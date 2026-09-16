#include "net_poller.h"

#include <Arduino.h>

#include <cstring>
#include <new>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
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
#include "transit_stats/events.h"
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
// Alerts/weather/bikes/liveness are OPTIONAL work (F12): none of them is what the screen is for,
// and every second one spends is a second the arrivals are older. A shorter timeout (and so, via
// http_fetch's 2x rule, a 16 s absolute ceiling instead of 30 s) keeps one sulking endpoint from
// eating a whole poll interval.
constexpr uint32_t kOptionalFetchTimeoutMs = 8000;
// Optional work is skipped entirely when the next transit poll is this close. Arrivals are the
// product; a weather refresh that makes them late is a bad trade at any price.
constexpr uint32_t kOptionalWorkReserveMs = 5000;
constexpr uint32_t kTaskStackBytes = 10240;
// Priority 0 - the same as the idle task - on purpose. HTTPClient waits for response headers in
// Stream::timedRead(), a busy loop with no yield, for up to the 15 s fetch timeout; at priority 1
// on core 0 that starves IDLE0, and this SDK's task watchdog (5 s, CONFIG_ESP_TASK_WDT_PANIC=y)
// then PANICS and reboots the board. Seen in the 2026-09-15 device suite as a reset with a
// backtrace ending in millis() <- timedRead() <- handleHeaderResponse(). At priority 0 FreeRTOS
// round-robins the poller with IDLE0 every tick, the watchdog is fed, and the poller still gets
// the CPU whenever nothing higher is runnable - which is the same as before, since everything
// else on the device runs above priority 0.
constexpr UBaseType_t kTaskPriority = 0;

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
// route_has_live_vehicles feeds the tracker's noshow-vs-outage rule, which is about 10-minute
// windows (DESIGN.md SS9.1), so a minute-old answer is as good as a fresh one - and this costs a
// TransitView request per route, which is exactly the kind of thing that must not sit between the
// user and their arrivals (F12).
constexpr uint32_t kLivenessRefreshMs = 60 * 1000;
// Past this, a cached liveness answer is not used at all: "we have not checked in five minutes"
// must read as "we do not know" (false, the conservative value - the tracker then folds a missing
// scheduled trip into an outage rather than claiming a no-show), never as "the route is quiet".
constexpr uint32_t kLivenessMaxAgeMs = 5 * 60 * 1000;

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

// ---- stats-page summary cache (F27, DESIGN.md SS8 "Stats page") -------------------------------
//
// Computing one of these streams a month of CSV off the SD card - seconds on a slow card, times
// the number of configured stops. getStopSummary() used to do that work INLINE, on whichever task
// asked, and the asker is ui/stats_screen.cpp running on the LVGL task: opening the stats page
// froze touch and the clock for as long as the card took. So the call is now non-blocking. It
// returns whatever is cached (saying how old that is, and whether anything is cached at all) and
// marks the entry for refresh; the poller task does the actual scanning in its idle slices, one
// stop per slice, so a five-stop refresh is spread over five slices instead of one long stall.
constexpr int kSummaryWindowDays = 30;
// Per-stop refresh floor. A summary over 30 days does not move meaningfully in ten minutes, and
// the scan is the single most expensive thing this device does.
constexpr uint32_t kSummaryRefreshMs = 10 * 60 * 1000;

struct SummaryCacheEntry {
  std::string stop_key;
  transit_stats::StopSummary summary;
  // DESIGN.md SS9.2: how many of those arrivals carry an inference marker. Kept next to the
  // summary rather than inside it because StopSummary is a library type and this is the only
  // caller that wants the number (StatsAggregator::inferredCount()).
  uint32_t inferred = 0;
  bool has_value = false;   // false: never computed, the screen must say "loading", not "0%"
  bool requested = true;    // a refresh is wanted (new entry, aged out, or config changed)
  uint32_t computed_ms = 0;
};
std::vector<SummaryCacheEntry> g_summary_cache;

// Fills `out_inferred` alongside the summary. Runs ONLY on the poller task (see above).
transit_stats::StopSummary computeStopSummary(const std::string &stop_key, uint32_t &out_inferred) {
  out_inferred = 0;
  transit::Epoch now = (transit::Epoch)time(nullptr);
  transit::Epoch start = now - (transit::Epoch)kSummaryWindowDays * 86400;
  // Heap-allocated: StatsAggregator's own header docs put sizeof() at just under 8KB, too large
  // to risk on the poller task's 10 KB stack alongside the rest of a poll cycle.
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
  out_inferred = agg->inferredCount();
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

// Everything the alerts cache already holds, with no network at all. This is what goes onto the
// first Snapshot of a cycle (F12): the ticker is already showing these, and withholding the
// arrivals until a 5-minute-cadence alerts fetch finishes trades the thing the device is for
// against the thing at the bottom of the screen.
std::vector<Alert> cachedAlerts() {
  std::vector<Alert> out;
  for (const auto &e : g_alerts_cache) {
    out.insert(out.end(), e.alerts.begin(), e.alerts.end());
  }
  return out;
}

// transit_core's HttpGet glue: http_fetch.cpp owns retry/backoff/TLS (DESIGN.md SS5), this just
// adapts its `const char*` signature to transit::HttpGet's `std::string` one and threads
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

// transit_core's completeness-aware transport (F13). Everything the poller fetches goes through
// this rather than the legacy HttpGet, because via HttpGet transit_core has to ASSUME a body
// arrived whole (adaptHttpGet, source.h) - and a TripUpdates feed cut off halfway still reports
// HTTP 200, so the stops the missing half would have filled came back empty and "successful".
// With a real FetchResult those stops are marked Health::ScheduleOnly with "live feed truncated"
// instead (septa_source.cpp), which is the difference between a quiet afternoon and a broken one.
transit::HttpGetEx makeHttpGetEx(uint32_t timeout_ms) {
  return [timeout_ms](const std::string &url,
                      std::function<bool(const uint8_t *, size_t)> onData) -> transit::FetchResult {
    // BusSchedules is tiny (~1 KB) and flaky, so buffer it and retry on the error shape before
    // handing the consumer a single clean delivery; everything else streams straight through.
    if (url.find("BusSchedules") != std::string::npos) {
      transit::FetchResult result;
      constexpr int kAttempts = 4;
      for (int attempt = 0; attempt < kAttempts; ++attempt) {
        std::vector<uint8_t> body;
        bool overflow = false;
        ReplyInfo reply;
        transit::FetchResult transport = transit_app::getEx(
            url.c_str(),
            [&](const uint8_t *d, size_t n) {
              if (body.size() + n > 4096) { overflow = true; return false; }
              body.insert(body.end(), d, d + n);
              return true;
            },
            timeout_ms, &reply);
        int status = transport.status;
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
          result.status = status;
          // This layer buffers the body before delivering it, so "complete" means the WHOLE body
          // was buffered: the transport reached the end of it AND our own 4 KB cap did not cut it
          // short. Reporting complete=true after an overflow would hand transit_core a truncated
          // schedule labelled good, which is how a stop ends up showing half its trips.
          result.complete = transport.complete && !overflow;
          result.bytes = body.size();
          if (!body.empty() && !onData(body.data(), body.size())) result.aborted = true;
          return result;
        }
        if (body.empty() && status > 0) {  // empty non-error reply: nothing to retry for
          result.status = status;
          result.complete = transport.complete;
          return result;
        }
        result.status = status;
        vTaskDelay(pdMS_TO_TICKS(400 * (attempt + 1)));
      }
      return result;
    }
    return transit_app::getEx(url.c_str(), std::move(onData), timeout_ms);
  };
}

// The legacy HttpGet form, for the collaborators that only want a status code (weather_service,
// bike_service - both buffer a bounded body and judge it themselves, so completeness adds
// nothing they can act on). One transport, two shapes: never two transports.
transit::HttpGet plainFrom(const transit::HttpGetEx &ex) {
  return [ex](const std::string &url, std::function<bool(const uint8_t *, size_t)> onData) -> int {
    return ex(url, std::move(onData)).status;
  };
}

// DESIGN.md SS4.7: alerts fetched per configured route/line, 5 min cadence, only when
// config.alerts. Kept separate from pollBusStops()/pollRailStops() because SeptaSource's alerts
// fetch is per-route rather than part of that orchestration (source.h) - see NOTES.md 7b for why
// the prefix differs per mode.
// `have_time` is checked before EVERY route's request (F12): a device with eight configured
// routes would otherwise commit to eight sequential fetches once the first one came due, and the
// arrivals behind them would age by however long that took.
std::vector<Alert> collectAlerts(const std::vector<StopConfig> &stops, bool alerts_enabled,
                                  const transit::HttpGetEx &http,
                                  const std::function<bool()> &have_time, bool *fetched_any) {
  if (fetched_any != nullptr) *fetched_any = false;
  if (!alerts_enabled) {
    bool had = !g_alerts_cache.empty();
    g_alerts_cache.clear();
    if (fetched_any != nullptr) *fetched_any = had;  // clearing the ticker is a change worth publishing
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
    if (have_time && !have_time()) break;  // out of budget: keep the cached alerts, poll instead

    std::vector<Alert> fetched;
    src.fetchAlertsEx(s.mode, s.route, &fetched, http);
    if (fetched_any != nullptr) *fetched_any = true;
    entry.fetched_ms = millis();
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
// SeptaSource::fetchTransitViewEx() per distinct bus/trolley route - the same public method
// pollBusStops uses - purely for this boolean.
//
// F12: it is now CACHED and scheduled like the other optional work, instead of running inline on
// every single poll. It used to add one TransitView round trip per route to the critical path
// between SEPTA and the screen, at the same 15-30 s cadence as the arrivals themselves, for a
// boolean that only decides whether a missed scheduled trip is written as a `noshow` or folded
// into an `outage` (DESIGN.md SS9.1) - a 10-minute-scale judgement.
struct LivenessEntry {
  std::string route;
  bool live = false;
  uint32_t fetched_ms = 0;
};
std::vector<LivenessEntry> g_liveness_cache;

void refreshRouteLiveness(const std::vector<StopConfig> &stops, const transit::HttpGetEx &http,
                          const std::function<bool()> &have_time) {
  transit::SeptaSource src;
  std::vector<std::string> live_keys;
  for (const auto &s : stops) {
    if (s.mode != Mode::Bus && s.mode != Mode::Trolley) continue;
    if (s.route.empty()) continue;
    if (std::find(live_keys.begin(), live_keys.end(), s.route) != live_keys.end()) continue;
    live_keys.push_back(s.route);

    LivenessEntry *entry = nullptr;
    for (auto &e : g_liveness_cache) {
      if (e.route == s.route) {
        entry = &e;
        break;
      }
    }
    if (entry != nullptr && entry->fetched_ms != 0 && (millis() - entry->fetched_ms) < kLivenessRefreshMs) continue;
    if (have_time && !have_time()) break;

    std::vector<TvVehicle> tv;
    src.fetchTransitViewEx(s.route, &tv, http);
    if (entry == nullptr) {
      g_liveness_cache.push_back({s.route, !tv.empty(), millis()});
    } else {
      entry->live = !tv.empty();
      entry->fetched_ms = millis();
    }
  }
  // Drop routes no longer configured, so a stale answer cannot outlive the stop it belonged to.
  g_liveness_cache.erase(std::remove_if(g_liveness_cache.begin(), g_liveness_cache.end(),
                                         [&](const LivenessEntry &e) {
                                           return std::find(live_keys.begin(), live_keys.end(), e.route) == live_keys.end();
                                         }),
                          g_liveness_cache.end());
}

// Unknown reads as false, which is the conservative answer: the tracker then treats a scheduled
// trip that never appeared as part of an outage rather than asserting the bus did not run.
bool routeIsLive(const std::string &route) {
  for (const auto &e : g_liveness_cache) {
    if (e.route != route) continue;
    if (e.fetched_ms == 0 || (millis() - e.fetched_ms) > kLivenessMaxAgeMs) return false;
    return e.live;
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

// DESIGN.md SS9.1 log schema v3: the `temp_c` column is CELSIUS, always. The forecast held here is
// in whatever unit the DISPLAY is configured for (weather.fahrenheit, DESIGN.md SS4.8), so this is
// the conversion transit_stats/events.h means when it says "the app layer must never log degrees
// Fahrenheit into this column". Before v3 the column simply carried the device unit with nothing
// in the row saying which, so the owner's month of logs was only interpretable by someone who also
// knew how the device had been set up.
int32_t toCelsius(double value, bool fahrenheit) {
  return (int32_t)lround(fahrenheit ? (value - 32.0) * 5.0 / 9.0 : value);
}

// DESIGN.md SS9.1 v2/v3 columns the tracker cannot know: the weather at the event (arrive/ghost/
// noshow rows, from the main forecast hour nearest ev.ts) and whether an alert (1) or a detour (2)
// applied to the route at that moment (every stop event).
void annotateEvents(std::vector<transit_stats::LogEvent> &events, const std::vector<transit::Alert> &alerts) {
  WeatherView wx = getWeather();
  for (auto &ev : events) {
    using transit_stats::EventType;
    if (ev.event == EventType::Bike || ev.event == EventType::Outage) continue;
    uint8_t flag = 0;
    for (const transit::Alert &al : alerts) {
      if (!al.current || al.route != ev.route) continue;
      flag = std::max<uint8_t>(flag, al.detours.empty() ? 1 : 2);
    }
    ev.alert = flag;
    // A stale forecast is not the weather at ev.ts; leaving temp/wx unset (null) says "unknown",
    // which is what DESIGN.md SS9.1's "empty means unknown, never coerced" asks for (F29).
    if (ev.event == EventType::Pred || !wx.enabled || wx.stale || !wx.main.valid()) continue;
    const weather::Hour *h = wx.main.at(ev.ts);
    if (h != nullptr && h->code >= 0) {
      ev.temp = toCelsius(h->temp, wx.fahrenheit);
      ev.wx = h->code;
    } else {
      ev.temp = toCelsius(wx.main.temp, wx.fahrenheit);
      ev.wx = wx.main.code;
    }
  }
}

// DESIGN.md SS9.1 `bike` rows: one per configured Indego station per local clock hour, taken from
// the last successful refresh (bike_service.cpp polls every 5 min, so a healthy sample is that
// fresh).
//
// F29 - a `bike` row is a claim that "this is what the docks held at ts", so three rules:
//   * Only log a sample we actually observed near `now`. bike_service deliberately keeps serving
//     the last good counts to the DISPLAY when the 400 KB feed fails (better than a blank panel),
//     but writing those into the log as this hour's observation is inventing data. A sample older
//     than kBikeSampleMaxAgeS is not logged at all.
//   * Never log the SAME fetch twice. The hour id alone did exactly that across an hour boundary:
//     one unchanged, hours-old sample became a fresh hourly observation every hour it survived.
//   * Unknown stays unknown. `ebikes` was coerced to 0 for a station whose feed entry has no
//     e-bike count, which reads back as "this station has no e-bikes" - the exact confusion
//     DESIGN.md SS9.1's "empty means unknown, never coerced to 0" exists to prevent.
constexpr uint32_t kBikeSampleMaxAgeS = 15 * 60;
int g_bike_logged_hour = -1;          // tm_yday * 24 + tm_hour of the last rows written
uint32_t g_bike_logged_epoch = 0;     // fetched_epoch of the sample those rows came from
void logBikeSamples(const Config &cfg, time_t now, const std::string &month) {
  if (!cfg.bike.enabled) return;
  BikeView bikes = getBikes();
  if (bikes.fetched_epoch == 0 || bikes.stations.empty()) return;
  // `now` is stamped at the start of the cycle and refreshBikes() ran inside it, so a sample
  // fetched seconds ago can carry a fetched_epoch slightly LATER than now. That is an age of
  // zero, not a reason to skip the freshest sample we will ever have.
  uint32_t now_u = (uint32_t)now;
  uint32_t age_s = bikes.fetched_epoch > now_u ? 0 : now_u - bikes.fetched_epoch;
  if (age_s > kBikeSampleMaxAgeS) return;
  if (bikes.fetched_epoch == g_bike_logged_epoch) return;
  struct tm lt;
  localtime_r(&now, &lt);
  int hour_id = lt.tm_yday * 24 + lt.tm_hour;
  if (hour_id == g_bike_logged_hour) return;
  bool wrote_any = false;
  for (const indego::Station &st : bikes.stations) {
    if (st.bikes < 0) continue;  // missing from the feed: nothing to sample
    transit_stats::LogEvent ev;
    ev.ts = (transit::Epoch)now;
    ev.event = transit_stats::EventType::Bike;
    ev.stop_key = "indego-" + std::to_string(st.id);
    // Vendor-supplied free text goes through the library's own sanitiser so the value written is
    // the value a reader gets back (transit_stats/events.h).
    ev.note = transit_stats::sanitizeLogField(st.name, transit_stats::kMaxCsvTextChars);
    ev.bikes = st.bikes;
    if (st.ebikes >= 0) ev.ebikes = st.ebikes;  // else: left unset = unknown, never 0
    if (st.docks >= 0) ev.docks = st.docks;
    if (appendLine(month.c_str(), transit_stats::toCsv(ev).c_str())) {
      wrote_any = true;
    } else {
      Serial.printf("[net_poller] SD write dropped a bike row for %s\n", ev.stop_key.c_str());
    }
  }
  // Only claim the hour once something landed: a card that was busy this cycle gets another try
  // on the next poll instead of losing the hour entirely.
  if (wrote_any) {
    g_bike_logged_hour = hour_id;
    g_bike_logged_epoch = bikes.fetched_epoch;
  }
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

// Publishes a Snapshot to every reader (UI, web server) and moves the LED with it. Called more
// than once per cycle (F12): the arrivals go out the moment they exist, and again when the alerts
// that belong to the same Snapshot arrive.
void publishSnapshot(const Snapshot &snap) {
  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    g_status.has_polled = true;
    g_status.ok = snap.last_poll_ok;
    g_status.last_http_status = snap.last_poll_ok ? 200 : 0;
    g_status.last_poll_epoch = (uint32_t)snap.generated;
    g_status.last_error = snap.last_error;
    g_snapshot = snap;
    xSemaphoreGive(g_mutex);
  }

  if (snap.last_poll_ok) {
    flashPollOk();
    setStatusLed(LedState::Off);
  } else {
    setStatusLed(LedState::Error);
  }
}

// DESIGN.md SS7 / model.h: last_poll_ok is "every stop's required sources succeeded" - which is
// just the AND of StopSnapshot::ok now that failure is reported per stop (septa_source.h) - and
// last_error is the FIRST failing stop's own error, not a global one. Recomputed over the merged
// stop list rather than taken from the two sub-snapshots so the two always agree with what the
// per-stop panels show.
void summarizePollHealth(Snapshot &snap) {
  snap.last_poll_ok = true;
  snap.last_error.clear();
  for (const StopSnapshot &s : snap.stops) {
    if (s.ok) continue;
    snap.last_poll_ok = false;
    if (snap.last_error.empty()) snap.last_error = s.error.empty() ? "stop unavailable" : s.error;
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

// Computes at most ONE requested stats summary, on the poller task, and stores it (F27). Returns
// true if it did work, so the idle loop can re-check its deadline afterwards.
//
// The cache mutex is deliberately NOT held across the scan: the scan is the slow part (a month of
// CSV off SD), and holding g_mutex through it would block getSnapshot() - i.e. the LVGL task's
// 1 Hz redraw - for exactly as long as the scan takes, which is the freeze this change exists to
// remove. Take, release, scan, take, store.
// Below this much free heap (or this small a largest block) the idle-slice work waits: a
// StatsAggregator is ~8 KB and a proxied body needs a 4 KB write buffer, and taking them while a
// PUT /api/config is being parsed on the web task is how "Unable to allocate FD" / 500s happened
// in the 2026-09-15 device suite. Nothing here is urgent; it runs on the next slice instead.
constexpr size_t kIdleWorkMinFreeHeap = 40 * 1024;
constexpr size_t kIdleWorkMinLargestBlock = 12 * 1024;
bool idleWorkHasHeadroom() {
  return ESP.getFreeHeap() >= kIdleWorkMinFreeHeap &&
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= kIdleWorkMinLargestBlock;
}

bool computeOneRequestedSummary() {
  std::string key;
  if (g_mutex == nullptr) return false;
  if (!idleWorkHasHeadroom()) return false;
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  for (const auto &e : g_summary_cache) {
    if (e.requested) {
      key = e.stop_key;
      break;
    }
  }
  xSemaphoreGive(g_mutex);
  if (key.empty()) return false;

  uint32_t inferred = 0;
  transit_stats::StopSummary summary = computeStopSummary(key, inferred);

  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    for (auto &e : g_summary_cache) {
      if (e.stop_key != key) continue;
      e.summary = summary;
      e.inferred = inferred;
      e.has_value = true;
      e.requested = false;
      e.computed_ms = millis();
      break;
    }
    xSemaphoreGive(g_mutex);
  }
  return true;
}

// Drops cached summaries for stops that are no longer configured (F27), so the stats page cannot
// keep answering for a stop the user removed.
void evictUnconfiguredSummaries(const std::vector<StopConfig> &stops) {
  if (g_mutex == nullptr) return;
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  g_summary_cache.erase(std::remove_if(g_summary_cache.begin(), g_summary_cache.end(),
                                        [&](const SummaryCacheEntry &e) {
                                          for (const auto &s : stops) {
                                            if (s.key == e.stop_key) return false;
                                          }
                                          return true;
                                        }),
                         g_summary_cache.end());
  xSemaphoreGive(g_mutex);
}

// One poll cycle. Returns the millis() deadline of the NEXT transit poll, which is also the
// budget the optional work below is measured against.
//
// F12 - the ORDER here is the whole point. This function used to fetch alerts, weather, bikes and
// one TransitView per route for the liveness flag, then feed the tracker and write to SD, and only
// after all of that publish the Snapshot. Every one of those is a network round trip on the same
// task, so a slow alerts endpoint and a 400 KB bike feed sat between SEPTA's answer and the
// screen: arrivals that were fetched at :00 reached the display at :12, and `generated` said :00,
// so the header cheerfully reported them as fresh. The arrivals are the product; they are
// published the instant they exist, and everything else runs afterwards on its own clock.
uint32_t pollOnce(uint32_t &consecutive_failures) {
  Config cfg = getActiveConfig();
  transit::HttpGetEx http = makeHttpGetEx(kFetchTimeoutMs);
  transit::HttpGetEx http_opt = makeHttpGetEx(kOptionalFetchTimeoutMs);
  transit::HttpGet http_opt_plain = plainFrom(http_opt);
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
  // C++ exceptions are on in this SDK (-fexceptions), and with nothing catching them a failed
  // std::vector growth inside the fetch/parse path ended in std::terminate -> abort -> reboot
  // (seen twice in the 2026-09-15 device suite: bad_alloc in pollBusStops while the web server
  // was busy and the largest free block was ~10 KB). A poll that cannot get memory is a failed
  // poll, reported per stop like any other outage, not a reset.
  Snapshot bus_snap, rail_snap;
  bool out_of_memory = false;
  try {
    bus_snap = transit::pollBusStops(bus_like, now, http, sched_cache);
    rail_snap = transit::pollRailStops(rail_like, now, http);
  } catch (const std::bad_alloc &) {
    out_of_memory = true;
  }
  if (out_of_memory) {
    Serial.printf("[net_poller] out of memory during the fetch (free %u, largest %u); reporting the poll as failed\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    bus_snap = Snapshot{};
    rail_snap = Snapshot{};
    for (const auto &s : cfg.stops) {
      StopSnapshot ss;
      ss.key = s.key;
      ss.fetched = now;
      ss.ok = false;
      ss.health = transit::Health::Unavailable;
      ss.error = "out of memory during fetch";
      bus_snap.stops.push_back(std::move(ss));
    }
  }

  Snapshot combined;
  combined.stops = std::move(bus_snap.stops);
  combined.stops.insert(combined.stops.end(), rail_snap.stops.begin(), rail_snap.stops.end());
  summarizePollHealth(combined);
  // Stamped HERE, not at the top of the function: `generated` is what the header's "updated 12 s
  // ago" and the staleness amber are measured from, so it has to be when the data became visible,
  // not when the cycle started.
  combined.generated = (transit::Epoch)time(nullptr);
  // Alerts belong to the Snapshot, and whatever the cache holds is what the ticker already shows;
  // publishing without them is never a reason to withhold the arrivals (F12). A second publish
  // follows below if a fetch brings new ones.
  combined.alerts = cachedAlerts();
  publishSnapshot(combined);

  // From here on everything is optional. The next transit poll's deadline is fixed first, so each
  // piece of work can ask whether it still has room rather than finding out afterwards.
  bool urgent = anyArrivalUrgent(combined, combined.generated);
  uint32_t interval_s = nextIntervalS(cfg, combined.last_poll_ok, urgent, consecutive_failures);
  if (g_last_poll_unsynced) interval_s = std::min<uint32_t>(interval_s, 10);  // re-poll soon once NTP lands
  const uint32_t deadline_ms = millis() + interval_s * 1000UL;
  // std::function, not auto: it is handed to collectAlerts()/refreshRouteLiveness() by const
  // reference, and an `auto` lambda would be wrapped into a fresh std::function (a heap
  // allocation) at each call site.
  const std::function<bool()> have_time = [deadline_ms]() {
    return (int32_t)(deadline_ms - millis()) > (int32_t)kOptionalWorkReserveMs;
  };

  if (have_time()) {
    bool alerts_fetched = false;
    combined.alerts = collectAlerts(cfg.stops, cfg.alerts, http_opt, have_time, &alerts_fetched);
    // Only when something actually came back: a cycle where every alert feed was still fresh must
    // not re-publish an identical Snapshot and reset the header's "updated N s ago".
    if (alerts_fetched) publishSnapshot(combined);  // same stops, now with the alerts for them
  }
  if (have_time()) refreshWeather(cfg, http_opt_plain);  // DESIGN.md SS4.8: 10 min per location
  if (have_time()) refreshBikes(cfg, http_opt_plain);    // DESIGN.md SS4.9: 5 min, 400 KB streamed

  // DESIGN.md SS9: feed every StopSnapshot to the tracker and append any resulting LogEvents to
  // the current month's CSV. This runs on EVERY cycle regardless of the budget - it is not
  // optional work. observe() is what maintains the outage bookkeeping and the two-consecutive-
  // -misses rule (tracker.h); skipping a cycle would make the tracker believe it saw a poll it
  // never saw. Only the liveness REFRESH above it is skippable, and an unknown liveness answers
  // false, which suppresses a noshow rather than inventing one.
  if (cfg.device.logging) {
    syncTrackerRegistrations(cfg.stops);
    refreshRouteLiveness(bus_like, http_opt, have_time);
    std::vector<transit_stats::LogEvent> events;
    for (const auto &stop : combined.stops) {
      const StopConfig *sc = nullptr;
      for (const auto &s : cfg.stops) {
        if (s.key == stop.key) {
          sc = &s;
          break;
        }
      }
      bool route_live = sc && routeIsLive(sc->route);
      // F17: the PER-STOP verdict, not the combined one. tracker.h is explicit that poll_ok is a
      // statement about THIS stop's data - passing the AND of every stop meant one stop's failing
      // schedule endpoint froze every other stop's inference too, and (worse, before per-stop
      // health existed) a stop whose own fetch failed could be observed as a successful poll with
      // no arrivals, which is how a network timeout produced `arrive` and `ghost` rows.
      if (tracker() != nullptr && now >= kSaneClockEpoch) tracker()->observe(stop, now, route_live, stop.ok, events);
    }
    if (now >= kSaneClockEpoch) {
      std::string month = currentLocalMonth();
      if (!events.empty()) {
        annotateEvents(events, combined.alerts);
        uint32_t dropped = 0;
        for (const auto &ev : events) {
          std::string line = transit_stats::toCsv(ev);
          if (!appendLine(month.c_str(), line.c_str())) dropped++;
        }
        if (dropped > 0) {
          // F24/F26: a row that did not reach the card is a hole in the statistics, not a
          // non-event. Say so where the owner can see it.
          SdStatus sd = getSdStatus();
          Serial.printf("[net_poller] SD dropped %u of %u log rows (%s); %u dropped since boot\n",
                        (unsigned)dropped, (unsigned)events.size(),
                        sd.error.empty() ? "unknown reason" : sd.error.c_str(), (unsigned)sd.dropped_rows);
        }
      }
      logBikeSamples(cfg, (time_t)now, month);
    }
  }

  evictUnconfiguredSummaries(cfg.stops);
  logHeapHeartbeat();
  return deadline_ms;
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
    // pollOnce() publishes the arrivals as soon as it has them and returns the deadline it set
    // for the next cycle (F12), so the interval is derived once, in the place that also budgets
    // the optional work against it.
    uint32_t deadline = pollOnce(consecutive_failures);

    // Blocks until the deadline, but wakes immediately if requestRepoll() gives the semaphore
    // (DESIGN.md SS7: PUT /api/config "triggers immediate re-poll").
    // Sleep in slices so queued web jobs (setup-wizard proxies, /api/stats) and the stats-page
    // summary scans run on this task instead of needing a stack of their own.
    while ((int32_t)(millis() - deadline) < 0) {
      if (xSemaphoreTake(g_wake_sem, pdMS_TO_TICKS(250)) == pdTRUE) break;
      // ONE job per slice, then look at the clock again (F12). `while (runQueuedProxyJob()) {}`
      // drained the whole queue back to back without ever checking it: two queued jobs - a
      // 400 KB Stops proxy and a 30-day stats scan are both seconds of work - could push the
      // next transit poll a long way past its deadline, and nothing in the loop noticed.
      if (idleWorkHasHeadroom() && runQueuedProxyJob() && (int32_t)(millis() - deadline) >= 0) break;
      // At most one stop's stats summary per slice, same reason (F27).
      if (computeOneRequestedSummary() && (int32_t)(millis() - deadline) >= 0) break;
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

void requestRepoll(bool data_changed) {
  if (data_changed) {
    g_invalidate_sched_cache = true;
    invalidateWeather();
    invalidateBikes();
  }
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

StopSummaryView getStopSummary(const std::string &stop_key) {
  StopSummaryView view;
  if (g_mutex == nullptr) return view;
  // Short wait, and give up rather than block: this runs on the LVGL task, which must never wait
  // on another task's lock (DESIGN.md SS5). A missed refresh costs one screen update.
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return view;
  uint32_t now_ms = millis();
  SummaryCacheEntry *entry = nullptr;
  for (auto &e : g_summary_cache) {
    if (e.stop_key == stop_key) {
      entry = &e;
      break;
    }
  }
  if (entry == nullptr) {
    // First ask for this stop: register it and let the poller do the scan. The caller gets
    // has_value = false, which the stats screen renders as "loading" - never as zeroes.
    g_summary_cache.push_back(SummaryCacheEntry{});
    entry = &g_summary_cache.back();
    entry->stop_key = stop_key;
    entry->requested = true;
  } else if (entry->has_value && (now_ms - entry->computed_ms) >= kSummaryRefreshMs) {
    entry->requested = true;  // aged out; the poller picks it up in its next idle slice
  }
  view.has_value = entry->has_value;
  view.pending = entry->requested;
  view.age_s = entry->has_value ? (now_ms - entry->computed_ms) / 1000u : 0;
  view.inferred = entry->inferred;
  view.summary = entry->summary;
  xSemaphoreGive(g_mutex);
  return view;
}

}  // namespace transit_app
