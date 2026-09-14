#include "net_poller.h"

#include <Arduino.h>

#include <cstring>
#include <new>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

#include "config_store.h"
#include "http_fetch.h"
#include "sd_logger.h"
#include "proxy_worker.h"
#include "status_led.h"
#include "transit_core/septa_source.h"
#include "transit_core/timeparse.h"
#include "transit_stats/aggregate.h"
#include "transit_stats/log_window.h"
#include "transit_stats/tracker.h"
#include "weather_service.h"
#include "bike_service.h"

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
constexpr uint32_t kTaskStackBytes = 10240;
constexpr UBaseType_t kTaskPriority = 1;

// DESIGN.md SS4.7.
constexpr uint32_t kBusSchedulesRefreshMs = 10 * 60 * 1000;
// A schedule that still looked like the wrong service day after every fetch attempt
// (transit_core septa_source.h fetchPlausibleSchedule) is kept only this long, so the next
// polls get another try at SEPTA's good backend instead of showing tomorrow's owl trips for
// ten minutes.
constexpr uint32_t kBusSchedulesSuspectRefreshMs = 2 * 60 * 1000;
constexpr uint32_t kAlertsRefreshMs = 5 * 60 * 1000;
constexpr int64_t kUrgentEtaS = 180;    // "any arrival is under 3 min" -> poll every 15s
constexpr uint32_t kUrgentIntervalS = 15;
constexpr uint32_t kBaseBackoffS = 30;
constexpr uint32_t kMaxBackoffS = 300;  // "cap 5 min"

SemaphoreHandle_t g_mutex = nullptr;
SemaphoreHandle_t g_wake_sem = nullptr;  // given to wake the poller task early (config change)

// NTP has synced once the clock reads later than 2023-11; before that every "minutes until"
// figure is measured from 1970 (first seen as "first trip in 29823220 min" on a boot where the
// first poll beat the NTP reply).
constexpr time_t kSaneClockEpoch = 1700000000;
bool clockIsSane() { return time(nullptr) >= kSaneClockEpoch; }
volatile bool g_last_poll_unsynced = false;

// Stand-in ScheduleCache for a poll that runs before the clock is sane: nothing is cached (a
// judgement made against 1970 would be wrong for ten minutes), so the next poll refetches.
class NoStoreScheduleCache : public transit::ScheduleCache {
 public:
  bool get(const std::string &, std::vector<SchedEntry> *) override { return false; }
  void put(const std::string &, const std::vector<SchedEntry> &) override {}
  void putSuspect(const std::string &, const std::vector<SchedEntry> &) override {}
};
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
        if (millis() - e.fetched_ms > e.ttl_ms) return false;
        *out = e.entries;
        return true;
      }
    }
    return false;
  }
  void put(const std::string &stop_id, const std::vector<SchedEntry> &entries) override {
    store(stop_id, entries, kBusSchedulesRefreshMs);
  }
  void putSuspect(const std::string &stop_id, const std::vector<SchedEntry> &entries) override {
    Serial.printf("[net_poller] BusSchedules for stop %s looks like the wrong service day (%u entries, first trip %ld min away); retrying in %u s\n",
                  stop_id.c_str(), (unsigned)entries.size(),
                  entries.empty() ? 0L : (long)((entries.front().scheduled - (transit::Epoch)time(nullptr)) / 60),
                  (unsigned)(kBusSchedulesSuspectRefreshMs / 1000));
    store(stop_id, entries, kBusSchedulesSuspectRefreshMs);
  }
  // DESIGN.md SS4.7: refresh "also on config change" - force every stop to be refetched on the
  // next poll rather than waiting out the 10 minute window.
  void invalidateAll() { entries_.clear(); }

 private:
  struct Entry {
    std::string stop_id;
    std::vector<SchedEntry> entries;
    uint32_t fetched_ms = 0;
    uint32_t ttl_ms = kBusSchedulesRefreshMs;
  };
  void store(const std::string &stop_id, const std::vector<SchedEntry> &entries, uint32_t ttl_ms) {
    for (auto &e : entries_) {
      if (e.stop_id == stop_id) {
        e.entries = entries;
        e.fetched_ms = millis();
        e.ttl_ms = ttl_ms;
        return;
      }
    }
    entries_.push_back({stop_id, entries, millis(), ttl_ms});
  }
  std::vector<Entry> entries_;
};

AppScheduleCache g_sched_cache;

// getStopSummary() cache (DESIGN.md SS8 "Stats page": on-time %, mean late, worst hour, ghost
// count, sample count, last 30 days). Recomputing this means streaming SD - fine once per UI
// refresh, not something to redo on every call from whatever task asks (ui/stats_screen.cpp, on
// the LVGL task). At most kMaxTrackedStops entries (one per configured stop), same bound as
// ArrivalTracker.
constexpr int kSummaryWindowDays = 30;
constexpr uint32_t kSummaryCacheMs = 60 * 1000;

struct SummaryCacheEntry {
  std::string stop_key;
  transit_stats::StopSummary summary;
  uint32_t computed_ms = 0;
};
std::vector<SummaryCacheEntry> g_summary_cache;

transit_stats::StopSummary computeStopSummary(const std::string &stop_key) {
  transit::Epoch now = (transit::Epoch)time(nullptr);
  transit::Epoch start = now - (transit::Epoch)kSummaryWindowDays * 86400;
  // Heap-allocated: StatsAggregator's own header docs put sizeof() at just under 8KB, too large
  // to risk on an arbitrary caller's stack (this can be called from the LVGL task, not just
  // net_poller's own - see net_poller.h).
  std::unique_ptr<transit_stats::StatsAggregator> agg(new (std::nothrow) transit_stats::StatsAggregator(stop_key, start, now));
  if (!agg) {
    Serial.println("[net_poller] StatsAggregator allocation failed; summary unavailable");
    return transit_stats::StopSummary{};
  }
  for (const std::string &month : transit_stats::monthsInWindow(start, now)) {
    streamLogLines(month + ".csv", [&](const char *line, size_t len) {
      agg->feedLine(line, len);
      return true;
    });
  }
  return transit_stats::summarize(*agg);
}

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
// True for SEPTA's {"error": ...} bodies (transit_core/NOTES.md): BusSchedules answers a valid
// stop_id with HTTP 400 + that body a few times in ten, then succeeds on retry.
bool isSeptaErrorBody(const std::vector<uint8_t> &b) {
  return b.size() >= 8 && memcmp(b.data(), "{\"error\"", 8) == 0;
}

// Minutes until the earliest upcoming "DateCalender" in a BusSchedules body, or -1 if none
// parsed. A cheap scan over the raw JSON (the entries are ~90 bytes each and there are at most a
// dozen) so the fetch layer can judge SEPTA's service day per backend and steer the sticky
// cookie before transit_core ever parses the body. Mirrors fetchPlausibleSchedule()'s test.
long firstUpcomingMinutes(const std::vector<uint8_t> &body) {
  static const char kKey[] = "\"DateCalender\":\"";
  const size_t klen = sizeof(kKey) - 1;
  transit::Epoch now = (transit::Epoch)time(nullptr);
  transit::Epoch best = 0;
  std::string text(body.begin(), body.end());
  size_t pos = 0;
  while ((pos = text.find(kKey, pos)) != std::string::npos) {
    pos += klen;
    size_t end = text.find('"', pos);
    if (end == std::string::npos) break;
    std::string raw = text.substr(pos, end - pos);
    std::string unescaped;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '/') continue;  // "09\/15\/26"
      unescaped.push_back(raw[i]);
    }
    bool ok = false;
    transit::Epoch t = transit::parseBusScheduleTime(unescaped, &ok);
    if (ok && t >= now - 60 && (best == 0 || t < best)) best = t;
    pos = end;
  }
  if (best == 0) return -1;
  return (long)((best - now) / 60);
}

std::string stopIdFromUrl(const std::string &url) {
  size_t p = url.find("stop_id=");
  return p == std::string::npos ? url : url.substr(p + 8);
}

transit::HttpGet makeHttpGet(bool tls_verify) {
  return [tls_verify](const std::string &url, std::function<bool(const uint8_t *, size_t)> onData) -> int {
    // BusSchedules is tiny (~1 KB) and flaky, so buffer it and retry on the error shape before
    // handing the consumer a single clean delivery; everything else streams straight through.
    if (url.find("BusSchedules") != std::string::npos) {
      int status = -1;
      constexpr int kAttempts = 4;
      for (int attempt = 0; attempt < kAttempts; ++attempt) {
        std::vector<uint8_t> body;
        bool overflow = false;
        ReplyInfo reply;
        status = transit_app::get(
            url.c_str(),
            [&](const uint8_t *d, size_t n) {
              if (body.size() + n > 4096) { overflow = true; return false; }
              body.insert(body.end(), d, d + n);
              return true;
            },
            kFetchTimeoutMs, tls_verify, &reply);
        if (overflow || (!body.empty() && !isSeptaErrorBody(body))) {
          // Judge the service day here as well as in transit_core (fetchPlausibleSchedule): only
          // this layer sees which backend answered, and only a fresh connection without the
          // sticky cookie can land on a different one (transit_core/NOTES.md 9).
          long first_min = clockIsSane() ? firstUpcomingMinutes(body) : -1;
          bool wrong_day = clockIsSane() && first_min > (long)(transit::kSchedulePlausibleS / 60);
          Serial.printf("[net_poller] BusSchedules stop %s: backend %s, first trip in %ld min%s\n",
                        stopIdFromUrl(url).c_str(), reply.backend.empty() ? "?" : reply.backend.c_str(), first_min,
                        wrong_day ? " (wrong service day)" : (clockIsSane() ? "" : " (clock not synced, not judged)"));
          if (!clockIsSane()) {
            // Can't tell a good backend from a bad one yet: deliver as-is, leave the cookie alone.
          } else if (wrong_day) {
            unpinScheduleBackend();
            if (attempt + 1 < kAttempts) {
              vTaskDelay(pdMS_TO_TICKS(200));
              continue;
            }
          } else {
            pinScheduleBackend(reply.set_cookie);
          }
          if (!body.empty()) onData(body.data(), body.size());
          return status;
        }
        if (body.empty() && status > 0) return status;  // empty non-error reply: nothing to retry for
        vTaskDelay(pdMS_TO_TICKS(400 * (attempt + 1)));
      }
      return status;
    }
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
volatile bool g_enabled = false;
bool g_task_created = false;

// May return nullptr: with exceptions disabled a plain `new` that fails calls std::terminate()
// (seen as a boot loop on the owner's board when the heap was too fragmented after Wi-Fi came
// up), so allocate with nothrow and let callers degrade to "no logging" instead of crashing.
// main.cpp calls preallocateTracker() before Wi-Fi starts so this normally never fails.
transit_stats::ArrivalTracker *tracker() {
  static bool warned = false;
  if (g_tracker == nullptr) {
    g_tracker = new (std::nothrow) transit_stats::ArrivalTracker();
    if (g_tracker == nullptr && !warned) {
      warned = true;
      Serial.printf("[net_poller] ArrivalTracker allocation failed (%u bytes); arrival logging disabled\n", (unsigned)sizeof(transit_stats::ArrivalTracker));
    }
  }
  return g_tracker;
}

void syncTrackerRegistrations(const std::vector<StopConfig> &stops) {
  transit_stats::ArrivalTracker *t = tracker();
  if (t == nullptr) return;
  for (const auto &s : stops) {
    t->registerStop(s.key, s.route, s.direction);
  }
}

void logHeapHeartbeat() {
  // DESIGN.md SS5: "log free heap once per poll at INFO". Printed unconditionally (not via
  // log_i()) because the flash-diet's CORE_DEBUG_LEVEL=2 (WARN) compiles log_i() out entirely
  // (firmware/README.md "Memory and flash budget") - this one line is deliberately always-on so
  // heap health stays visible over serial regardless of debug verbosity.
  size_t free_heap = ESP.getFreeHeap();
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf("[net_poller] free_heap=%u largest_block=%u stack_free=%u\n", (unsigned)free_heap, (unsigned)largest,
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
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

  // Before NTP: use a cache that stores nothing, so schedules judged against 1970 are refetched
  // on the next poll (which comes sooner - see pollerTask) instead of sticking for ten minutes.
  NoStoreScheduleCache nostore;
  g_last_poll_unsynced = !clockIsSane();
  transit::ScheduleCache &sched_cache = g_last_poll_unsynced ? static_cast<transit::ScheduleCache &>(nostore)
                                                             : static_cast<transit::ScheduleCache &>(g_sched_cache);
  Snapshot bus_snap = transit::pollBusStops(bus_like, now, http, sched_cache);
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
  refreshWeather(cfg, http);  // DESIGN.md SS4.8: 10 min cadence per location, no-op otherwise
  refreshBikes(cfg, http);    // DESIGN.md SS4.9: 5 min cadence, streams the 400 KB feed

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
      if (tracker() != nullptr && now >= kSaneClockEpoch) tracker()->observe(stop, now, route_live, combined.last_poll_ok, events);
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
  // Created before Wi-Fi so the stack comes from an unfragmented heap; polling starts when
  // startNetPoller() flips g_enabled.
  while (!g_enabled) {
    xSemaphoreTake(g_wake_sem, pdMS_TO_TICKS(500));
  }
  // Wait for NTP before the first poll (up to 45 s): the tracker and the SD log key everything
  // by wall-clock time, a poll at "3 seconds since 1970" produced garbage pred rows, and the
  // BusSchedules service-day check needs today's date. 20 s was not always enough.
  for (int i = 0; i < 90 && !clockIsSane(); ++i) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  if (!clockIsSane()) {
    Serial.println("[net_poller] clock not synced after 45 s; polling anyway, schedules are not cached and logging waits for a sane clock");
  }
  uint32_t consecutive_failures = 0;
  for (;;) {
    pollOnce();

    Config cfg = getActiveConfig();
    Snapshot snap = getSnapshot();
    bool ok = snap.last_poll_ok;
    bool urgent = anyArrivalUrgent(snap, (transit::Epoch)time(nullptr));
    uint32_t interval_s = nextIntervalS(cfg, ok, urgent, consecutive_failures);
    if (g_last_poll_unsynced) interval_s = std::min<uint32_t>(interval_s, 10);  // re-poll soon once NTP lands

    // Blocks for up to interval_s, but wakes immediately if requestRepoll() gives the semaphore
    // (DESIGN.md SS7: PUT /api/config "triggers immediate re-poll").
    // Sleep in slices so queued web jobs (setup-wizard proxies, /api/stats) run on this task
    // instead of needing a stack of their own; a config change (requestRepoll) ends the wait.
    uint32_t deadline = millis() + interval_s * 1000UL;
    while ((int32_t)(millis() - deadline) < 0) {
      if (xSemaphoreTake(g_wake_sem, pdMS_TO_TICKS(250)) == pdTRUE) break;
      while (runQueuedProxyJob()) {
      }
    }
  }
}

}  // namespace

bool preallocateTracker() {
  return tracker() != nullptr;
}

void initNetPoller() {
  if (g_mutex == nullptr) {
    g_mutex = xSemaphoreCreateMutex();
  }
  if (g_wake_sem == nullptr) {
    g_wake_sem = xSemaphoreCreateBinary();
  }
  if (!g_task_created) {
    g_task_created = xTaskCreatePinnedToCore(pollerTask, "net_poller", kTaskStackBytes, nullptr, kTaskPriority, nullptr, 0 /* core 0, DESIGN.md SS5 */) == pdPASS;
  }
}

void startNetPoller(uint32_t poll_seconds) {
  g_poll_seconds = poll_seconds > 0 ? poll_seconds : 30;
  initNetPoller();
  syncTrackerRegistrations(getActiveConfig().stops);
  g_enabled = true;
  xSemaphoreGive(g_wake_sem);
}

void requestRepoll() {
  g_invalidate_sched_cache = true;
  invalidateWeather();
  invalidateBikes();
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
  uint32_t now_ms = millis();
  for (auto &e : g_summary_cache) {
    if (e.stop_key != stop_key) continue;
    if (now_ms - e.computed_ms >= kSummaryCacheMs) {
      e.summary = computeStopSummary(stop_key);
      e.computed_ms = now_ms;
    }
    out = e.summary;
    return true;
  }
  SummaryCacheEntry entry;
  entry.stop_key = stop_key;
  entry.summary = computeStopSummary(stop_key);
  entry.computed_ms = now_ms;
  out = entry.summary;
  g_summary_cache.push_back(std::move(entry));
  return true;
}

}  // namespace transit_app
