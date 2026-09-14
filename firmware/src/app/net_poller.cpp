#include "net_poller.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <ctime>
#include <utility>
#include <vector>

#include "config_store.h"
#include "http_fetch.h"
#include "sd_logger.h"
#include "status_led.h"
#include "transit_core/septa_source.h"
#include "transit_stats/tracker.h"

using transit::Alert;
using transit::Mode;
using transit::SchedEntry;
using transit::Snapshot;
using transit::StopConfig;
using transit::StopSnapshot;
using transit::TvVehicle;

namespace transit_app {

namespace {

constexpr uint32_t kFetchTimeoutMs = 15000;
constexpr uint32_t kTaskStackBytes = 12288;
constexpr UBaseType_t kTaskPriority = 1;

// DESIGN.md SS4.7.
constexpr uint32_t kBusSchedulesRefreshMs = 10 * 60 * 1000;
constexpr uint32_t kAlertsRefreshMs = 5 * 60 * 1000;
constexpr int64_t kUrgentEtaS = 180;    // "any arrival is under 3 min" -> poll every 15s
constexpr uint32_t kUrgentIntervalS = 15;
constexpr uint32_t kBaseBackoffS = 30;
constexpr uint32_t kMaxBackoffS = 300;  // "cap 5 min"

SemaphoreHandle_t g_mutex = nullptr;
SemaphoreHandle_t g_wake_sem = nullptr;  // given to wake the poller task early (config change)
transit::Snapshot g_snapshot;
PollStatus g_status;
uint32_t g_poll_seconds = 30;
volatile bool g_invalidate_sched_cache = false;

// ---- BusSchedules cache (DESIGN.md SS4.7: "cached 10 min", "also on config change") ----------
// At most kMaxStops (8, config_store.h) distinct stop_ids, matching the config's own cap; a
// linear scan over <=8 entries is cheaper than a map for this size.
class AppScheduleCache : public transit::ScheduleCache {
 public:
  bool get(const std::string &stop_id, std::vector<SchedEntry> *out) override {
    for (auto &e : entries_) {
      if (e.stop_id == stop_id) {
        if (millis() - e.fetched_ms > kBusSchedulesRefreshMs) return false;
        *out = e.entries;
        return true;
      }
    }
    return false;
  }
  void put(const std::string &stop_id, const std::vector<SchedEntry> &entries) override {
    for (auto &e : entries_) {
      if (e.stop_id == stop_id) {
        e.entries = entries;
        e.fetched_ms = millis();
        return;
      }
    }
    entries_.push_back({stop_id, entries, millis()});
  }
  // DESIGN.md SS4.7: refresh "also on config change" - force every stop to be refetched on the
  // next poll rather than waiting out the 10 minute window.
  void invalidateAll() { entries_.clear(); }

 private:
  struct Entry {
    std::string stop_id;
    std::vector<SchedEntry> entries;
    uint32_t fetched_ms = 0;
  };
  std::vector<Entry> entries_;
};

AppScheduleCache g_sched_cache;

// Keyed by septaAlertsUrl(); a plain vector rather than std::map since there are at most a
// handful of distinct alert feeds (DESIGN.md SS6 caps config at 8 stops) and this project already
// avoids std::map elsewhere for the same reason (see transit_stats's own fixed-array containers) -
// a linear scan over <=8 entries costs nothing at runtime and skips std::map's flash-heavy
// red-black-tree template instantiation for a container this small.
struct AlertCacheEntry {
  std::string url;
  std::vector<Alert> alerts;
  uint32_t fetched_ms = 0;
};
std::vector<AlertCacheEntry> g_alerts_cache;

AlertCacheEntry &findOrCreateAlertsEntry(const std::string &url) {
  for (auto &e : g_alerts_cache) {
    if (e.url == url) return e;
  }
  g_alerts_cache.push_back({url, {}, 0});
  return g_alerts_cache.back();
}

// transit_core's HttpGet glue: http_fetch.cpp owns retry/backoff/TLS (DESIGN.md SS5), this just
// adapts its `const char*` signature to transit::HttpGet's `std::string` one and threads
// config.device.tls_verify through.
transit::HttpGet makeHttpGet(bool tls_verify) {
  return [tls_verify](const std::string &url, std::function<bool(const uint8_t *, size_t)> onData) -> int {
    return transit_app::get(url.c_str(), std::move(onData), kFetchTimeoutMs, tls_verify);
  };
}

// DESIGN.md SS4.7: alerts fetched per configured route/line, 5 min cadence, only when
// config.alerts. Kept separate from pollBusStops()/pollRailStops() because SeptaSource's alerts
// fetch is per-route rather than part of that orchestration (source.h) - see NOTES.md 7b for why
// the prefix differs per mode.
std::vector<Alert> collectAlerts(const std::vector<StopConfig> &stops, bool alerts_enabled,
                                  const transit::HttpGet &http) {
  if (!alerts_enabled) {
    g_alerts_cache.clear();
    return {};
  }

  transit::SeptaSource src;
  uint32_t now_ms = millis();
  std::vector<std::string> live_keys;

  for (const auto &s : stops) {
    std::string url = transit::septaAlertsUrl(s.mode, s.route);
    if (url.empty()) continue;
    if (std::find(live_keys.begin(), live_keys.end(), url) != live_keys.end()) continue;  // already handled this cycle
    live_keys.push_back(url);

    AlertCacheEntry &entry = findOrCreateAlertsEntry(url);
    bool stale = entry.fetched_ms == 0 || (now_ms - entry.fetched_ms) >= kAlertsRefreshMs;
    if (!stale) continue;

    std::vector<Alert> fetched;
    src.fetchAlerts(s.mode, s.route, &fetched, http);
    entry.fetched_ms = now_ms;
    entry.alerts = std::move(fetched);  // replaces even with an empty result - matches SEPTA's
                                         // own "no current alerts" being indistinguishable from a
                                         // transient miss (NOTES.md SS4/7b); the alerts ticker is
                                         // low-stakes enough not to need a separate retry policy
  }

  // Drop entries no longer configured so a removed route's stale alert doesn't linger forever.
  g_alerts_cache.erase(std::remove_if(g_alerts_cache.begin(), g_alerts_cache.end(),
                                       [&](const AlertCacheEntry &e) {
                                         return std::find(live_keys.begin(), live_keys.end(), e.url) == live_keys.end();
                                       }),
                        g_alerts_cache.end());

  std::vector<Alert> out;
  for (const auto &e : g_alerts_cache) {
    out.insert(out.end(), e.alerts.begin(), e.alerts.end());
  }
  return out;
}

// route_has_live_vehicles (transit_stats/tracker.h) needs a route-level "is TransitView tracking
// any vehicle on this route right now" signal that pollBusStops() (septa_source.h) fetches
// internally but doesn't expose (it only returns the merged per-stop Snapshot). Rather than
// duplicate pollBusStops' own orchestration/parsing to get at it, this does one extra
// SeptaSource::fetchTransitView() per distinct bus/trolley route - the same public method
// pollBusStops uses - purely for this boolean. Routes are typically 1-2 for this device (DESIGN.md
// SS6 caps at 8 stops total), so the extra ~1-3KB request per route per poll cycle is a bounded,
// deliberate cost documented in firmware/README.md rather than a parsing-logic duplication.
std::vector<std::pair<std::string, bool>> routeLiveness(const std::vector<StopConfig> &stops,
                                                          const transit::HttpGet &http) {
  std::vector<std::pair<std::string, bool>> live;
  transit::SeptaSource src;
  for (const auto &s : stops) {
    if (s.mode != Mode::Bus && s.mode != Mode::Trolley) continue;
    if (s.route.empty()) continue;
    bool already = false;
    for (const auto &kv : live) {
      if (kv.first == s.route) {
        already = true;
        break;
      }
    }
    if (already) continue;
    std::vector<TvVehicle> tv;
    src.fetchTransitView(s.route, &tv, http);
    live.push_back({s.route, !tv.empty()});
  }
  return live;
}

bool routeIsLive(const std::vector<std::pair<std::string, bool>> &live, const std::string &route) {
  for (const auto &kv : live) {
    if (kv.first == route) return kv.second;
  }
  return false;
}

bool anyArrivalUrgent(const Snapshot &snap, transit::Epoch now) {
  for (const auto &stop : snap.stops) {
    for (const auto &a : stop.arrivals) {
      transit::Epoch eff = a.effective();
      if (eff > 0 && (eff - now) < kUrgentEtaS) return true;
    }
  }
  return false;
}

std::string currentLocalMonth() {
  time_t now = time(nullptr);
  struct tm local_tm;
  localtime_r(&now, &local_tm);
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d-%02d", local_tm.tm_year + 1900, local_tm.tm_mon + 1);
  return std::string(buf);
}

// One long-lived ArrivalTracker across the device's uptime (DESIGN.md SS9.1); registerStop() is
// idempotent so it's safe to re-run on every config change. Heap-allocated on first use rather
// than a plain global/static instance: ArrivalTracker's own header doc puts sizeof(ArrivalTracker)
// at ~14-16KB (kMaxTrackedStops * StopState, each holding a kMaxTrackedTripsPerStop array), which
// is fine for the heap (~200KB free at boot per firmware/README.md) but overflows the ESP32's
// fixed static .bss/.data budget if declared as a file-scope object - confirmed by hitting
// "DRAM segment data does not fit" at link time with it declared that way.
transit_stats::ArrivalTracker *g_tracker = nullptr;

transit_stats::ArrivalTracker &tracker() {
  if (g_tracker == nullptr) {
    g_tracker = new transit_stats::ArrivalTracker();
  }
  return *g_tracker;
}

void syncTrackerRegistrations(const std::vector<StopConfig> &stops) {
  for (const auto &s : stops) {
    tracker().registerStop(s.key, s.route, s.direction);
  }
}

void logHeapHeartbeat() {
  // DESIGN.md SS5: "log free heap once per poll at INFO". Printed unconditionally (not via
  // log_i()) because the flash-diet's CORE_DEBUG_LEVEL=2 (WARN) compiles log_i() out entirely
  // (firmware/README.md "Memory and flash budget") - this one line is deliberately always-on so
  // heap health stays visible over serial regardless of debug verbosity.
  size_t free_heap = ESP.getFreeHeap();
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf("[net_poller] free_heap=%u largest_block=%u\n", (unsigned)free_heap, (unsigned)largest);
}

void pollOnce() {
  Config cfg = getActiveConfig();
  transit::HttpGet http = makeHttpGet(cfg.device.tls_verify);
  transit::Epoch now = (transit::Epoch)time(nullptr);

  if (g_invalidate_sched_cache) {
    g_invalidate_sched_cache = false;
    g_sched_cache.invalidateAll();
  }

  std::vector<StopConfig> bus_like, rail_like;
  for (const auto &s : cfg.stops) {
    (s.mode == Mode::Rail ? rail_like : bus_like).push_back(s);
  }

  Snapshot bus_snap = transit::pollBusStops(bus_like, now, http, g_sched_cache);
  Snapshot rail_snap = transit::pollRailStops(rail_like, now, http);

  Snapshot combined;
  combined.generated = now;
  combined.last_poll_ok = bus_snap.last_poll_ok && rail_snap.last_poll_ok;
  combined.last_error = !bus_snap.last_poll_ok ? bus_snap.last_error
                         : !rail_snap.last_poll_ok ? rail_snap.last_error
                                                    : "";
  combined.stops = std::move(bus_snap.stops);
  combined.stops.insert(combined.stops.end(), rail_snap.stops.begin(), rail_snap.stops.end());
  combined.alerts = collectAlerts(cfg.stops, cfg.alerts, http);

  // DESIGN.md SS9: feed every StopSnapshot to the tracker and append any resulting LogEvents to
  // the current month's CSV. Done before the mutex swap below so a slow SD write never holds up
  // readers of the live Snapshot.
  if (cfg.device.logging) {
    syncTrackerRegistrations(cfg.stops);
    std::vector<std::pair<std::string, bool>> live_by_route = routeLiveness(bus_like, http);
    std::vector<transit_stats::LogEvent> events;
    for (const auto &stop : combined.stops) {
      const StopConfig *sc = nullptr;
      for (const auto &s : cfg.stops) {
        if (s.key == stop.key) {
          sc = &s;
          break;
        }
      }
      bool route_live = sc && routeIsLive(live_by_route, sc->route);
      tracker().observe(stop, now, route_live, combined.last_poll_ok, events);
    }
    if (!events.empty()) {
      std::string month = currentLocalMonth();
      for (const auto &ev : events) {
        std::string line = transit_stats::toCsv(ev);
        appendLine(month.c_str(), line.c_str());
      }
    }
  }

  logHeapHeartbeat();

  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    g_status.has_polled = true;
    g_status.ok = combined.last_poll_ok;
    g_status.last_http_status = combined.last_poll_ok ? 200 : 0;
    g_status.last_poll_epoch = (uint32_t)now;
    g_status.last_error = combined.last_error;
    g_snapshot = combined;
    xSemaphoreGive(g_mutex);
  }

  if (combined.last_poll_ok) {
    flashPollOk();
    setStatusLed(LedState::Off);
  } else {
    setStatusLed(LedState::Error);
  }
}

// Computes how long to sleep before the next cycle, per DESIGN.md SS4.7: the configured
// poll_seconds normally, 15s when an arrival is imminent, or exponential backoff (30/60/120s,
// capped at 5 min) after a failed cycle.
uint32_t nextIntervalS(const Config &cfg, bool ok, bool urgent, uint32_t &consecutive_failures) {
  if (!ok) {
    consecutive_failures = std::min<uint32_t>(consecutive_failures + 1, 4);
    uint32_t backoff = kBaseBackoffS << (consecutive_failures - 1);
    return std::min(backoff, kMaxBackoffS);
  }
  consecutive_failures = 0;
  return urgent ? std::min<uint32_t>(cfg.device.poll_seconds, kUrgentIntervalS) : cfg.device.poll_seconds;
}

void pollerTask(void * /*arg*/) {
  uint32_t consecutive_failures = 0;
  for (;;) {
    pollOnce();

    Config cfg = getActiveConfig();
    Snapshot snap = getSnapshot();
    bool ok = snap.last_poll_ok;
    bool urgent = anyArrivalUrgent(snap, (transit::Epoch)time(nullptr));
    uint32_t interval_s = nextIntervalS(cfg, ok, urgent, consecutive_failures);

    // Blocks for up to interval_s, but wakes immediately if requestRepoll() gives the semaphore
    // (DESIGN.md SS7: PUT /api/config "triggers immediate re-poll").
    xSemaphoreTake(g_wake_sem, pdMS_TO_TICKS(interval_s * 1000UL));
  }
}

}  // namespace

void startNetPoller(uint32_t poll_seconds) {
  g_poll_seconds = poll_seconds > 0 ? poll_seconds : 30;
  if (g_mutex == nullptr) {
    g_mutex = xSemaphoreCreateMutex();
  }
  if (g_wake_sem == nullptr) {
    g_wake_sem = xSemaphoreCreateBinary();
  }
  syncTrackerRegistrations(getActiveConfig().stops);
  xTaskCreatePinnedToCore(pollerTask, "net_poller", kTaskStackBytes, nullptr, kTaskPriority, nullptr, 0 /* core 0, DESIGN.md SS5 */);
}

void requestRepoll() {
  g_invalidate_sched_cache = true;
  if (g_wake_sem != nullptr) {
    xSemaphoreGive(g_wake_sem);
  }
}

transit::Snapshot getSnapshot() {
  transit::Snapshot copy;
  if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    copy = g_snapshot;
    xSemaphoreGive(g_mutex);
  }
  return copy;
}

PollStatus getPollStatus() {
  PollStatus copy;
  if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    copy = g_status;
    xSemaphoreGive(g_mutex);
  }
  return copy;
}

bool getStopSummary(const std::string &stop_key, transit_stats::StopSummary &out) {
  (void)stop_key;
  (void)out;
  // Implemented in the logging/stats milestone alongside StatsAggregator wiring - see
  // web_server.cpp's GET /api/stats handler, which streams the CSV itself rather than going
  // through this accessor (a StopSummary needs a StatsAggregator fed from the log, which only
  // makes sense to build on demand from a request handler, not kept live in the poller task).
  return false;
}

}  // namespace transit_app
