#include "net_poller.h"

#include <Arduino.h>

#include <cstring>
#include <new>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "config_store.h"
#include "cxx_exception_pool.h"
#include "heap_reserve.h"
#include "heap_trace.h"
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
#include "ui_lock.h"
#include "weather_service.h"
#include "bike_service.h"
#include "web_server.h"  // otaBusy(): the wedge counter stands down during a firmware upload

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
#ifdef TRANSIT_HTTPS
// A TLS handshake runs on this task: mbedTLS keeps ~1 KB of RSA-verify scratch, 512 B of
// ssl_client's own buffer and ECP temporaries on the stack, 3-4 KB in all (DESIGN.md SS2.1).
// The plain-http build's high-water mark is 4,556 B free of 10 KB (measured 2026-09-16), which
// is exactly that margin and no more, so the HTTPS build gives the task 2 KB extra.
// Caveat worth knowing before trusting this number: on the owner's board no handshake ever got
// past mbedtls_ssl_setup() (the heap ran out first), so the 12 KB was measured at 6,328 B free
// with plain fetches and the 3-4 KB of handshake stack has never actually been spent here. The
// SDK-rebuild test pass has to re-measure it (DESIGN.md SS2.1 "Forced gate").
constexpr uint32_t kTaskStackBytes = 12288;
#else
constexpr uint32_t kTaskStackBytes = 10240;
#endif
// Priority 1 (above IDLE0, below the ESP-IDF network/timer tasks) - the original value. It was
// briefly dropped to 0 to stop HTTPClient's header busy-wait (Stream::timedRead, no yield to a
// lower priority) from starving IDLE0 and tripping the 5 s task watchdog, but priority 0 made the
// poller the lowest task on core 0: under concurrent web load it was starved while holding the
// snapshot mutex, so the display task's 1 s getSnapshot() wait timed out and asserted in
// vTaskPriorityDisinheritAfterTimeout (device suite, 2026-09-15). The watchdog is instead cured at
// its source - a single stream read now times out well under 5 s (kStreamReadTimeoutMs in
// http_fetch.cpp) - so the poller can stay at priority 1 and never starves the mutex.
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
// Published as a shared, immutable pointer rather than as a value (2026-09-16). A reader used to
// copy the whole Snapshot - vectors of arrivals, a string per field - with g_mutex held, so the
// cost of every /api/state and every 1 Hz redraw was charged to everyone else waiting for the
// lock, and the display task's copy was also the allocation that aborted the board on 2026-09-15
// (main.cpp loop()'s comment names it: bad_alloc in Snapshot::operator= <- getSnapshot()
// <- ui::tick()). Now the poller builds the new Snapshot outside the lock and swaps the pointer in,
// and a reader's work under the lock is one refcount bump. The display task no longer copies the
// Snapshot at all - it holds a reference to the poller's own - which is one whole Snapshot less
// live heap, and one less per-second allocate/free cycle fragmenting it.
std::shared_ptr<const transit::Snapshot> g_snapshot;
PollStatus g_status;
uint32_t g_poll_seconds = 30;
volatile bool g_invalidate_sched_cache = false;

// ---- Liveness stamps (DESIGN.md SS12.1, net_poller.h "Liveness") -----------------------------
// Written by the poller task at the end of every cycle whatever the outcome, and at the start of
// every fetch it makes; read lock-free by main.cpp's display loop, which is the only task that can
// notice this one has stopped. Aligned 32-bit words and plain bools, like g_alerts_fetched_ms
// above: no mutex, because the reader is the display task and it must never wait on the poller's
// lock (DESIGN.md SS5).
volatile bool g_liveness_armed = false;
volatile bool g_before_first_cycle = true;
volatile uint32_t g_cycle_end_ms = 0;
volatile uint32_t g_progress_ms = 0;
volatile uint32_t g_cycle_interval_ms = 30000;

// "The poller is still going round." Called from makeHttpGetEx()'s lambda, so every network round
// trip a cycle makes - transit, rail, alerts, weather, bikes - refreshes it. Without this the net
// could only see whole cycles, and a legitimate cycle on a blackholing network runs for minutes
// (poller_liveness.h): the window needed to cover one would have been too wide to catch a freeze.
// One 32-bit store, on the poller task, no allocation, cannot throw.
inline void notePollerProgress() { g_progress_ms = millis(); }

// How long a NON-DISPLAY task may wait for g_mutex, and nothing else. None of these is the display
// task's budget: that one is zero, it is applied by takeShared() (ui_lock.h) rather than by any
// call site, and no number here can change it. Do not read kSummaryWaitMs as "the UI waits 50 ms" -
// that reading is the bug DESIGN.md SS12.1's second entry is about.
constexpr uint32_t kSnapshotWaitMs = 1000;
constexpr uint32_t kStatusWaitMs = 1000;
constexpr uint32_t kSummaryWaitMs = 50;

// ---- Self-heal restart note (DESIGN.md SS12.1) ------------------------------------------------
// RTC slow memory: kept across ESP.restart() (and across a panic), not across a power cycle, which
// is exactly the lifetime wanted - "the last boot rebooted itself, here is why". The magic word is
// what distinguishes a real note from the garbage RTC RAM holds after power-on, and it is cleared
// as soon as the note is read so a later unrelated reset cannot inherit a stale one.
constexpr uint32_t kNoteMagic = 0x50544452;  // 'PTDR'
struct RtcSelfHealNote {
  uint32_t magic;
  uint32_t reason;
  uint32_t uptime_s;
  uint32_t a;
  uint32_t b;
};
RTC_NOINIT_ATTR RtcSelfHealNote g_rtc_note;
RestartNote g_prev_note;

// Reads the RTC note into g_prev_note once per boot and disarms it. Called from initNetPoller()
// (early in setup(), before Wi-Fi) and lazily from getRestartNote() so the order cannot matter.
void captureRestartNote() {
  static bool done = false;
  if (done) return;
  done = true;
  if (g_rtc_note.magic == kNoteMagic) {
    uint32_t r = g_rtc_note.reason;
    g_prev_note.reason = r <= (uint32_t)kSelfHealMax ? (SelfHeal)r : SelfHeal::None;
    g_prev_note.uptime_s = g_rtc_note.uptime_s;
    g_prev_note.a = g_rtc_note.a;
    g_prev_note.b = g_rtc_note.b;
  }
  g_rtc_note.magic = 0;
}

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

// ---- The poll cycle's working set (DESIGN.md SS5) --------------------------------------------
// Allocated once by preallocatePollBuffers(), from main.cpp, before Wi-Fi - the same reasoning and
// the same moment as the ArrivalTracker above. transit_core's PollBuffers owns the GTFS-RT entity
// buffer (4,096 B), its retention block (~4,600 B), one response body buffer (4,096 B) and the
// BusSchedules parse block (24 x 128 B); g_sched_raw below is the firmware's own, because the
// BusSchedules branch of makeHttpGetEx() buffers the response at the TRANSPORT layer - before
// transit_core sees it - so it cannot be the same vector transit_core is appending into.
//
// Every one of these is a multi-kilobyte contiguous request, they all fall inside the same few
// hundred milliseconds of a cycle, and the largest free block on this board rests at 25-28 KB and
// decays with uptime and request rate (DESIGN.md SS12.1). At 11.7 KB - measured, with 36 KB still
// free - the cycle threw std::bad_alloc and every cycle after it did the same. Reserved before
// Wi-Fi, out of a heap that is still one run, none of them is ever asked for again.
//
// Heap-allocated rather than a file-scope object, like the tracker: the ESP32's static .bss budget
// is separate from and much smaller than the heap. A null pointer is a working fallback, not a
// failure - transit_core builds them per call exactly as it used to.
transit::PollBuffers *g_poll_buffers = nullptr;
std::vector<uint8_t> g_sched_raw;

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
// millis() of the newest alerts fetch, 0 = none; read by getAlertsStatus() from the LVGL task. A
// single aligned 32-bit store/load is atomic on the ESP32, so no mutex, and the cache itself
// (which the poller task mutates freely) is never touched from outside this task.
volatile uint32_t g_alerts_fetched_ms = 0;

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
// parsed. The scan itself is transit_core's (septa.h firstUpcomingScheduleTime) and runs over the
// body vector IN PLACE. It used to start with `std::string text(body.begin(), body.end())` - a
// second full copy of a body capped at 4 KB, i.e. a second contiguous block of up to 4 KB asked
// for while the GTFS-RT entity buffer, the retention buffer and the body itself were all still
// live (audit_runtime SS3 #12, ranked recommendation 7) - plus two small strings per entry.
long firstUpcomingMinutes(const std::vector<uint8_t> &body) {
  transit::Epoch now = (transit::Epoch)time(nullptr);
  transit::Epoch best = transit::firstUpcomingScheduleTime(body.data(), body.size(), now);
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
        // The transport-level buffer, reserved once before Wi-Fi (g_sched_raw): the cap below is
        // 4 KB, so with the reservation in place the insert() loop never reallocates and never
        // asks for a contiguous block mid-cycle. clear() keeps the capacity.
        std::vector<uint8_t> &body = g_sched_raw;
        body.clear();
        bool overflow = false;
        ReplyInfo reply;
        notePollerProgress();
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
        // A TRANSPORT failure is not what these retries are for, and repeating them multiplies a
        // dead network by four. transit_app::getEx() has already spent its three attempts, its
        // 0.5 s + 1 s of backoff and (on a blackholing network) a DNS timeout per attempt on this
        // exact URL; status <= 0 means none of that reached a server. What this loop exists for is
        // a SEPTA *error body* - a real reply from a backend holding the wrong service day - and
        // that always comes back with a status. Before this check, one stop's schedule cost up to
        // 12 URL fetches per cycle (4 here x 3 in fetchPlausibleSchedule), which on a blackholing
        // network is minutes per stop and was half of why the liveness net could reboot a healthy
        // board mid-cycle (poller_liveness.h).
        if (status <= 0) break;
        vTaskDelay(pdMS_TO_TICKS(400 * (attempt + 1)));
      }
      return result;
    }
    notePollerProgress();
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
    g_alerts_fetched_ms = 0;
    if (fetched_any != nullptr) *fetched_any = had;  // clearing the ticker is a change worth publishing
    return {};
  }

  transit::SeptaSource src(g_poll_buffers);  // the shared response body buffer, not a fresh one
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
    g_alerts_fetched_ms = entry.fetched_ms;
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
  transit::SeptaSource src(g_poll_buffers);  // the shared response body buffer, not a fresh one
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
// the last successful refresh (bike_service.cpp polls every 10 min, so a healthy sample is that
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
// at ~8 KB on the target (kMaxTrackedStops * StopState, each holding a kMaxTrackedTripsPerStop
// array; it was ~12 KB before the slot count dropped from 12 to 8 in 0.3.1), which
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
  // Two "free" numbers on purpose (found 2026-09-16, DESIGN.md SS2.1): ESP.getFreeHeap() is
  // heap_caps_get_free_size(MALLOC_CAP_INTERNAL), which on the classic ESP32 includes the IRAM heap
  // region that is only 32-bit addressable - about 34 KB on this build that malloc() will never hand
  // out for a buffer, a string or a TLS record. `free8` is what an ordinary allocation can actually
  // get (MALLOC_CAP_8BIT), and `largest_block` was already measured in those terms.
  size_t free_heap = ESP.getFreeHeap();
  size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  Serial.printf("[net_poller] free_heap=%u free8=%u largest_block=%u stack_free=%u\n", (unsigned)free_heap, (unsigned)free8,
                (unsigned)largest, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// DESIGN.md SS2.1. One sample per stage of pollOnce, so the question "is there a point in the poll
// cycle where a TLS session would fit?" - and, on this branch, "which stage takes the largest block
// from 23.5 KB to under 12 KB, and which one leaks on the failure path?" - is answered by
// measurement instead of by moving fetches around and hoping. The numbers are MALLOC_CAP_8BIT -
// what an allocation can really get - and are directly comparable with `need` in the [https] gate
// lines and with tlsHeapNeed() in /api/state.
//
// CHANGED ON THIS BRANCH (2026-09-17): the sample itself is now unconditional and goes into
// heap_trace.cpp's .bss ring, which GET /api/debug/ui reads out. Serial cannot be captured on this
// bench - opening the port resets the board - so a trace that only exists as Serial output is a
// measurement that cannot be taken. The Serial LINE is unchanged and still behind
// -DTRANSIT_HEAP_TRACE, so the -https envs keep printing exactly what DESIGN.md SS2.1's figures
// were read off. Cost when the flag is off: two heap_caps_ calls and a 12-byte store per stage.
void tracePoll(uint8_t stage) {
  heapTraceMark(stage);
#ifdef TRANSIT_HEAP_TRACE
  Serial.printf("[poll-heap] %-13s free8=%u largest=%u\n", kHeapTraceStages[stage],
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
}

// transit_core calls this through the function pointer in source.h, so the three big allocations
// inside pollBusStops() (the GTFS-RT buffers, each route's TransitView, each stop's BusSchedules)
// get their own stage rather than being hidden inside one pre/post pair. Installed by
// initNetPoller(); the library holds nothing but the pointer.
void transitCoreTrace(int stage) {
  switch (stage) {
    case transit::kPollTraceRtStream: heapTraceMark(kStageRtStream); break;
    case transit::kPollTraceTransitView: heapTraceMark(kStageTvRoute); break;
    case transit::kPollTraceSchedStop: heapTraceMark(kStageSchedStop); break;
    case transit::kPollTraceMerge: heapTraceMark(kStageMerge); break;
    default: break;
  }
}

// ---- Instrument counters (diag branch) ------------------------------------------------------
// How many Snapshot objects published by publishSnapshot() are still alive. The audit's M1 found
// THREE: the poller's own `combined`, the published copy of it, and the display task's last-good
// reference - each 4-8 KB. Since the publish became a move (publishSnapshot() below) the poller's
// `combined` and the published object are the same object, so the expected reading is ONE between
// publishes and TWO for up to a second after one, while the display's last-good still names the
// outgoing Snapshot. A number above two, or one that climbs, is a reference nobody is releasing.
// Counted by
// construction and destruction of the published object itself rather than by a custom deleter, so
// make_shared's single allocation is preserved: a `shared_ptr(new T, deleter)` would have added a
// second block per publish, which is the instrument changing the thing it measures. The counter is
// touched from whichever task drops the last reference, hence atomic.
std::atomic<uint32_t> g_snapshots_live{0};
struct CountedSnapshot : Snapshot {
  explicit CountedSnapshot(Snapshot &&s) : Snapshot(std::move(s)) { g_snapshots_live.fetch_add(1); }
  ~CountedSnapshot() { g_snapshots_live.fetch_sub(1); }
};

// Cycles that reported failure, and the live value of pollerTask's wedge tally. Both are read by
// GET /api/debug/ui: `failed_polls` says how far into the failure loop the board is, and
// `wedged_polls` says how close the self-heal reboot is - the two numbers that place a trace
// sample in the ~10 minute cycle the board is repeating.
volatile uint32_t g_failed_polls = 0;
volatile uint32_t g_wedged_polls = 0;

// Publishes a Snapshot to every reader (UI, web server) and moves the LED with it. Called more
// than once per cycle (F12): the arrivals go out the moment they exist, and again when the alerts
// that belong to the same Snapshot arrive.
//
// TAKES ITS ARGUMENT BY RVALUE, and that is the point (audit_runtime SS2/M1, ranked
// recommendation 3). It used to take `const Snapshot&` and COPY it into the shared_ptr, so the
// caller's `combined` and the published object were two distinct Snapshots - 4-8 KB each, six of
// their blocks being 1.1-2.3 KB contiguous arrival vectors - and `combined` then stayed alive
// through the whole optional tail: the alerts fetch, the weather fetch, the 400 KB Indego stream,
// the tracker pass and every SD write. Moving it means there is ONE Snapshot: the caller is left
// holding the returned pointer, not a second object.
//
// Returns the published pointer so the tail can read the stops and alerts it has just handed over.
// That is a refcount, not a copy.
std::shared_ptr<const Snapshot> publishSnapshot(Snapshot &&snap) {
  // Everything that allocates happens HERE, with nothing held: make_shared's single block and the
  // error string. This used to run with g_mutex held, which is what made a reader's wait long
  // enough to matter in the first place - the display task was not waiting for a pointer, it was
  // waiting out somebody else's allocation of a few kilobytes of arrivals. A bad_alloc escapes
  // before the lock is ever taken and is caught by pollOnce()/pollerTask() exactly as before,
  // leaving the published Snapshot untouched.
  // CountedSnapshot, not Snapshot: same object, same single make_shared allocation, but its ctor
  // and dtor move g_snapshots_live so /api/debug/ui can say how many published Snapshots are alive
  // (diag branch). The shared_ptr is converted to shared_ptr<const Snapshot> immediately; the
  // control block still destroys the real type, so the non-virtual destructor is not a problem.
  std::shared_ptr<const Snapshot> next = std::make_shared<const CountedSnapshot>(std::move(snap));
  // Read off the published object, not off `snap`: `snap` has been moved from and owns nothing.
  PollStatus status;
  status.has_polled = true;
  status.ok = next->last_poll_ok;
  status.last_http_status = next->last_poll_ok ? 200 : 0;
  status.last_poll_epoch = (uint32_t)next->generated;
  status.last_error = next->last_error;
  const bool ok = next->last_poll_ok;
  std::shared_ptr<const Snapshot> published = next;  // a refcount bump; `next` is emptied below

  // `previous` takes the outgoing Snapshot out of the critical section so its destructor - freeing
  // those same vectors and strings - also runs with the lock released.
  std::shared_ptr<const Snapshot> previous;
  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    // Two pointer swaps and a struct swap whose only non-trivial member is a string being moved
    // between two objects. Nothing here allocates, frees or can throw, so there is no try/catch
    // around it any more: the old one existed only because the copy above used to be inside.
    previous.swap(g_snapshot);
    g_snapshot.swap(next);
    std::swap(g_status, status);
    xSemaphoreGive(g_mutex);
  }

  if (ok) {
    flashPollOk();
    setStatusLed(LedState::Off);
  } else {
    setStatusLed(LedState::Error);
  }
  return published;
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
// StatsAggregator is ~8 KB and taking it while a PUT /api/config is being parsed on the web task is
// how "Unable to allocate FD" / 500s happened in the 2026-09-15 device suite. Nothing here is
// urgent; it runs on the next slice instead.
//
// Both halves read MALLOC_CAP_8BIT since 2026-09-16 (DESIGN.md SS2.1). The free half was
// ESP.getFreeHeap() = MALLOC_CAP_INTERNAL, which includes ~34 KB of 32-bit-word-only IRAM heap, so
// a 40 KB INTERNAL floor was asking for about 6 KB of the heap an allocation can actually use and
// almost never fired - the largest-block half had been doing the work alone. 16 KB is what the
// slice really takes: the ~8 KB contiguous StatsAggregator (heap-allocated precisely because it is
// too big for a task stack, proxy_worker.cpp) plus the same again for the CSV scan's strings and
// whatever the web task is holding at that moment. The block half stays at 12 KB, 1.5x that 8 KB
// object. The 4 KB proxy write buffer the old comment cited is not in this sum: it is
// `static uint8_t wbuf[4096]` in proxy_worker.cpp and never comes off the heap at all.
// 12,020 and not 12 * 1024: largest-block sizes land on a 512-byte lattice at offset 500, so a
// round 12 KB sits 12 B above a real resting value of 12,276 and would refuse it by a hair while
// the next value up clears by 1,012 B. `756 + 512k` is mid-gap, 256 B from either neighbour.
// web_server.cpp's kMinOtaLargestBlock carries the full explanation and the measurements.
constexpr size_t kIdleWorkMinFree8 = 16 * 1024;
constexpr size_t kIdleWorkMinLargestBlock = 12020;
bool idleWorkHasHeadroom() {
  return heap_caps_get_free_size(MALLOC_CAP_8BIT) >= kIdleWorkMinFree8 &&
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
  heapTraceBeginCycle();  // bumps the cycle number, then stamps kStagePollStart
#ifdef TRANSIT_HEAP_TRACE
  Serial.printf("[poll-heap] %-13s free8=%u largest=%u\n", kHeapTraceStages[kStagePollStart],
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
  // Clears the working set for this cycle and gives back an oversized body buffer, if some
  // unusually large response grew one last cycle. Done HERE, at poll-start, because this is where
  // the largest free block is at its best (DESIGN.md SS5).
  if (g_poll_buffers != nullptr) g_poll_buffers->beginCycle();
  // Take the error-reply reserve back if a 503 spent it and the heap has recovered since
  // (heap_reserve.h). Poll-start is one of the two contexts this is allowed from - the other is
  // the idle slice below - because neither is the handler that released it, and both are on a task
  // that can afford to be told "not yet".
  rearmHeapReserveIfSafe();
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
  tracePoll(kStagePreTransit);
  try {
    bus_snap = transit::pollBusStops(bus_like, now, http, sched_cache, g_poll_buffers);
    rail_snap = transit::pollRailStops(rail_like, now, http, g_poll_buffers);
  } catch (const std::bad_alloc &) {
    // Stamped INSIDE the catch, before anything unwound by the throw has been rebuilt: with the
    // per-stage marks above it, the ring then reads "...sched-stop, oom-transit", which names the
    // allocation that failed rather than only the call that contained it (diag branch).
    heapTraceMark(kStageOomTransit);
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

  // Everything the tail needs out of `combined` that is not the Snapshot itself, read BEFORE it is
  // handed over: after the move it owns nothing.
  const bool poll_ok = combined.last_poll_ok;
  const bool urgent = anyArrivalUrgent(combined, combined.generated);

  // Published by MOVE (audit_runtime SS2/M1). The poller keeps the pointer, not a second Snapshot:
  // `combined` used to stay alive here until the end of the function, i.e. across the alerts
  // fetch, the weather fetch, the 400 KB Indego stream, the tracker pass and every SD write, while
  // the published copy of the same 4-8 KB - six of whose blocks are 1.1-2.3 KB contiguous arrival
  // vectors - sat beside it.
  std::shared_ptr<const Snapshot> published = publishSnapshot(std::move(combined));
  tracePoll(kStagePostTransit);
  if (!poll_ok) g_failed_polls++;

  // From here on everything is optional. The next transit poll's deadline is fixed first, so each
  // piece of work can ask whether it still has room rather than finding out afterwards.
  uint32_t interval_s = nextIntervalS(cfg, poll_ok, urgent, consecutive_failures);
  if (g_last_poll_unsynced) interval_s = std::min<uint32_t>(interval_s, 10);  // re-poll soon once NTP lands
  const uint32_t deadline_ms = millis() + interval_s * 1000UL;
  // The liveness window is a multiple of the interval the poller is ACTUALLY running at, backoff
  // included (DESIGN.md SS12.1), so it is published from here - the one place that interval is
  // decided - rather than re-derived from a deadline somewhere else.
  g_cycle_interval_ms = interval_s * 1000UL;
  // std::function, not auto: it is handed to collectAlerts()/refreshRouteLiveness() by const
  // reference, and an `auto` lambda would be wrapped into a fresh std::function (a heap
  // allocation) at each call site.
  const std::function<bool()> have_time = [deadline_ms]() {
    return (int32_t)(deadline_ms - millis()) > (int32_t)kOptionalWorkReserveMs;
  };

  if (have_time()) {
    bool alerts_fetched = false;
    tracePoll(kStagePreAlerts);
    std::vector<Alert> fresh_alerts = collectAlerts(cfg.stops, cfg.alerts, http_opt, have_time, &alerts_fetched);
    // Only when something actually came back: a cycle where every alert feed was still fresh must
    // not re-publish an identical Snapshot and reset the header's "updated N s ago".
    if (alerts_fetched) {
      // The second publish of the cycle, and the one place a copy of the stops is unavoidable: the
      // published Snapshot is const and shared (the display task holds a reference to it for up to
      // a second), so the alerts cannot be written into it. What changed is that this is now ONE
      // copy, made and handed over in the same breath, instead of a second copy of a `combined`
      // the poller was going to hold for the rest of the cycle anyway. It runs at most once every
      // kAlertsRefreshMs (5 min), and only when a feed actually answered.
      Snapshot with_alerts;
      with_alerts.generated = published->generated;
      with_alerts.last_poll_ok = published->last_poll_ok;
      with_alerts.last_error = published->last_error;
      with_alerts.stops = published->stops;  // may throw bad_alloc; pollerTask() catches it
      with_alerts.alerts = std::move(fresh_alerts);
      published = publishSnapshot(std::move(with_alerts));
    }
    tracePoll(kStagePostAlerts);
  }
  if (have_time()) {
    tracePoll(kStagePreWeather);
    refreshWeather(cfg, http_opt_plain);  // DESIGN.md SS4.8: 10 min per location
    tracePoll(kStagePostWeather);
  }
  if (have_time()) {
    tracePoll(kStagePreBikes);
    refreshBikes(cfg, http_opt_plain);    // DESIGN.md SS4.9: 10 min, 400 KB streamed
    tracePoll(kStagePostBikes);
  }

  // DESIGN.md SS9: feed every StopSnapshot to the tracker and append any resulting LogEvents to
  // the current month's CSV. This runs on EVERY cycle regardless of the budget - it is not
  // optional work. observe() is what maintains the outage bookkeeping and the two-consecutive-
  // -misses rule (tracker.h); skipping a cycle would make the tracker believe it saw a poll it
  // never saw. Only the liveness REFRESH above it is skippable, and an unknown liveness answers
  // false, which suppresses a noshow rather than inventing one.
  if (cfg.device.logging) {
    syncTrackerRegistrations(cfg.stops);
    tracePoll(kStagePreLiveness);
    refreshRouteLiveness(bus_like, http_opt, have_time);
    tracePoll(kStagePostLiveness);
    std::vector<transit_stats::LogEvent> events;
    // The stops the poller published a moment ago, read through the pointer it kept rather than
    // out of a second copy it was holding for exactly this (audit_runtime SS2).
    for (const auto &stop : published->stops) {
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
    tracePoll(kStagePostTracker);  // `events` is built by now; the tracker itself is a fixed block
    if (now >= kSaneClockEpoch) {
      std::string month = currentLocalMonth();
      if (!events.empty()) {
        // The alerts on the Snapshot that is live right now - which is the list the second publish
        // above put there when a feed answered, and the cached list otherwise. collectAlerts()'s
        // return value differs from this only by entries for routes that are no longer configured,
        // and annotateEvents() matches alerts to events BY ROUTE, so an unconfigured route has no
        // event to annotate: the flag it writes is the same either way.
        annotateEvents(events, published->alerts);
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
    tracePoll(kStagePostSd);  // after every toCsv()/appendLine() and the Indego samples
  }

  evictUnconfiguredSummaries(cfg.stops);
  logHeapHeartbeat();
  tracePoll(kStageCycleEnd);
  return deadline_ms;
}

void pollerTask(void * /*arg*/) {
  // DESIGN.md SS12.1: pay this task's one-time __cxa_eh_globals allocation now, while the heap is
  // still untouched. Without it the FIRST throw on this task does a plain malloc inside __cxa_throw
  // and calls std::terminate if it fails - and this task's first throw is, by construction, the
  // bad_alloc the catch blocks below exist for. Covers the queued proxy and stats jobs too: they
  // run on this task, not one of their own.
  transit_app::warmExceptionGlobals("net_poller");
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
  // Self-heal for a wedged heap (found 2026-09-15). A no-PSRAM ESP32 whose heap has been
  // fragmented into tiny pieces - e.g. by a long burst of rapid config saves, each of which
  // rebuilds the whole LVGL screen, interleaved with active polling - can reach a state where
  // ~50 KB is free but the largest block is ~2 KB, too small for any fetch buffer. Every poll then
  // fails with a caught bad_alloc (the board stays up and honest, but shows nothing new), and
  // nothing defragments a running heap. The one recovery is what a person would do: power-cycle.
  //
  // WHAT THIS DOES NOT COVER (established on hardware 2026-09-16, DESIGN.md SS12.1): it counts
  // cycles that COMPLETE AND REPORT FAILURE. It sits after pollOnce() returns, so a poller that
  // stops completing cycles at all never reaches it and the counter freezes rather than climbing;
  // and the `else` below zeroes the tally whenever either condition lapses, so one cycle whose
  // largest block bounced back over the threshold wipes fourteen. That gap is why the liveness
  // stamp below exists and why main.cpp's display loop watches it. The two are independent nets:
  // this one catches a heap that has wedged while the poller still runs, that one catches a
  // poller that has stopped. Neither replaces the other.
  //
  // Guard tightly so this only ever fires on a genuine wedge, never on an ordinary SEPTA outage:
  //   * largest block below kWedgeLargestBlock (a normal idle board sits ~20-30 KB) - a SEPTA
  //     outage leaves the heap healthy, so that case keeps its normal backoff and never reboots;
  //   * AND that condition held across kWedgePollsBeforeReboot consecutive failed polls, which
  //     with the failure backoff is several minutes, so a brief blip cannot trigger it.
  constexpr size_t kWedgeLargestBlock = 6 * 1024;
  constexpr uint32_t kWedgePollsBeforeReboot = 15;
  uint32_t wedged_polls = 0;
  for (;;) {
    // pollOnce() publishes the arrivals as soon as it has them and returns the deadline it set
    // for the next cycle (F12), so the interval is derived once, in the place that also budgets
    // the optional work against it. The whole cycle is wrapped as a backstop against std::bad_alloc:
    // pollOnce() catches it around the arrival fetch itself (to set per-stop "out of memory"
    // health), but the optional tail - route liveness, alerts, weather, bikes, SD logging - can
    // also allocate, and with -fexceptions on an uncaught throw there is std::terminate = reboot
    // (seen 2026-09-15: bad_alloc in fetchTransitViewEx from refreshRouteLiveness). Caught here it
    // is just a short-retry failed cycle; if the heap is genuinely wedged the counter below reboots.
    uint32_t deadline;
    try {
      deadline = pollOnce(consecutive_failures);
    } catch (const std::bad_alloc &) {
      // The optional tail (alerts, weather, bikes, liveness, SD) has no catch of its own, so this
      // is where a throw from any of them lands. Stamped here, the ring's previous entry names
      // which of them was live - "pre-bikes, oom-cycle" is the Indego stream, "post-weather,
      // oom-cycle" is the liveness fetch, and so on (diag branch).
      heapTraceMark(kStageOomCycle);
      g_failed_polls++;
      Serial.printf("[net_poller] out of memory during the poll cycle (free %u, largest %u); short retry\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      consecutive_failures = std::min<uint32_t>(consecutive_failures + 1, 4);
      deadline = millis() + 15000;
      // pollOnce() threw before it set g_cycle_interval_ms, so the previous cycle's interval
      // stands. Deliberate: it is the longer, safer number for the liveness window below.
    }

    // The cycle stamp (DESIGN.md SS12.1). Written here, on the ONE path every cycle takes, whatever
    // happened inside it - a good poll, a failed poll, a caught bad_alloc. It says "the poller is
    // still going round", which is the thing the wedge counter below cannot say: that counter only
    // advances on cycles that COMPLETE AND REPORT FAILURE, so a poller that stops completing cycles
    // at all freezes it at whatever it was. Nothing here can throw or block.
    //
    // g_progress_ms is reset with it, so the two are equal between cycles and notePollerProgress()
    // only ever moves it forward from here. The net judges the later of the two: a cycle can
    // legitimately take minutes on a blackholing network, so "a cycle completed" cannot be the only
    // evidence the poller is alive (poller_liveness.h has the arithmetic).
    g_cycle_end_ms = millis();
    g_progress_ms = g_cycle_end_ms;
    g_before_first_cycle = false;

    // HAZARD: a firmware upload, exactly as in main.cpp's liveness net. An OTA takes the heap for
    // the length of a ~1.7 MB write, which is precisely the condition this counter looks for, and
    // `wedged_polls` carries across an upload, so a device already near the threshold could restart
    // itself mid-Update.write(). That is not a brick - the boot partition only switches at
    // Update.end(true), so a half-written inactive slot is inert and the device comes back on the
    // image it already had - but it throws away the owner's upload at the worst moment and looks
    // like a crash. Stand down while one is running, and forget the count rather than resume it:
    // whatever the heap was doing before the upload is not evidence about what it is doing after.
    //
    // Wedge detection otherwise (see above): a failed poll while the largest free block is
    // critically small. getPollStatus() reflects what pollOnce() just published.
    if (otaBusy()) {
      wedged_polls = 0;
    } else if (!getPollStatus().ok && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kWedgeLargestBlock) {
      if (++wedged_polls >= kWedgePollsBeforeReboot) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        Serial.printf("[net_poller] heap wedged: %u consecutive failed polls with largest block < %u B (free %u); rebooting to recover\n",
                      (unsigned)wedged_polls, (unsigned)kWedgeLargestBlock, (unsigned)ESP.getFreeHeap());
        noteSelfHealRestart(SelfHeal::HeapWedge, wedged_polls, (uint32_t)largest);
        Serial.flush();
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP.restart();
      }
    } else {
      wedged_polls = 0;
    }
    g_wedged_polls = wedged_polls;  // diag branch: mirrored so /api/debug/ui can read it

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
      //
      // Each completed job is also a liveness stamp. A job is the poller doing something, and a
      // long one (a 400 KB proxy fetch, a month of CSV through the aggregator) would otherwise sit
      // inside the window with nothing refreshing it. The loop itself cannot livelock - it exits on
      // a deadline the previous cycle computed from a bounded interval - so stamping here cannot
      // hide a stuck poller.
      //
      // The gate is passed IN rather than asked first (audit_runtime SS4, ranked recommendation 2).
      // `idleWorkHasHeadroom() && runQueuedProxyJob()` short-circuited, so on a heap that could no
      // longer clear the gate the queue was never even read: the job sat there for the rest of the
      // device's uptime holding a paused request whose server-side timeout the library had turned
      // off, and once both slots were held every later stats/proxy request got a permanent "proxy
      // worker busy". Dequeuing is always allowed; the gate now decides only whether the job is
      // RUN or answered 503, which the client retries.
      rearmHeapReserveIfSafe();  // see pollOnce(): the other safe context, four times a second
      if (runQueuedProxyJob(idleWorkHasHeadroom())) {
        notePollerProgress();
        if ((int32_t)(millis() - deadline) >= 0) break;
      }
      // At most one stop's stats summary per slice, same reason (F27).
      if (computeOneRequestedSummary()) {
        notePollerProgress();
        if ((int32_t)(millis() - deadline) >= 0) break;
      }
    }
  }
}

}  // namespace

bool preallocateTracker() {
  return tracker() != nullptr;
}

bool preallocatePollBuffers() {
  if (g_poll_buffers == nullptr) {
    g_poll_buffers = new (std::nothrow) transit::PollBuffers();
    if (g_poll_buffers != nullptr) {
      try {
        g_poll_buffers->reserveAll();
      } catch (const std::bad_alloc &) {
        // Partially reserved is still usable - whatever was not reserved is simply grown on
        // demand, which is the old behaviour. Nothing here may throw past setup().
        Serial.println("[net_poller] poll working set only partly reserved (out of memory)");
      }
    }
  }
  // The transport's own BusSchedules buffer (see g_sched_raw). 4 KB is the cap makeHttpGetEx
  // enforces, so reserving it here means that branch never reallocates.
  try {
    g_sched_raw.reserve(4096);
  } catch (const std::bad_alloc &) {
    Serial.println("[net_poller] BusSchedules buffer not reserved (out of memory)");
  }
  return g_poll_buffers != nullptr;
}

void initNetPoller() {
  captureRestartNote();  // before Wi-Fi, before anything else can reset the board
  // diag branch: lets transit_core stamp the heap at its own stage boundaries (source.h). One
  // function-pointer store, done before any poll can run.
  transit::setPollTraceHook(&transitCoreTrace);
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
  // Arm the liveness net from HERE, not from boot: this is the moment polling is expected to
  // happen. Everything before it - LittleFS, the Wi-Fi captive portal, a provisioned device
  // retrying a router that is not there - is time the poller is deliberately not running, and
  // connectWifiOrPortal() does not even return during the portal, so loop() is not yet running to
  // ask. g_before_first_cycle keeps the extra boot grace on until a cycle has actually finished.
  g_cycle_end_ms = millis();
  g_progress_ms = g_cycle_end_ms;
  g_cycle_interval_ms = g_poll_seconds * 1000UL;
  g_before_first_cycle = true;
  // Only if there IS a poller task. If xTaskCreatePinnedToCore() failed there is nothing to stamp
  // the liveness clock and nothing a reboot would fix, so arming would be a guaranteed loop of
  // "boot, wait out the window, restart" - the one failure mode this net must not create.
  g_liveness_armed = g_task_created;
  g_enabled = true;
  xSemaphoreGive(g_wake_sem);
}

PollerLiveness getPollerLiveness() {
  PollerLiveness lv;
  const uint32_t now = millis();
  lv.armed = g_liveness_armed;
  lv.before_first_cycle = g_before_first_cycle;
  lv.since_ms = now - g_cycle_end_ms;  // unsigned: correct across the 49-day millis() wrap
  // The LATER of the two stamps, i.e. the SMALLER age. g_progress_ms is set to g_cycle_end_ms at
  // every cycle end, so this is never larger than since_ms; the min() is belt and braces against a
  // torn read of two words the poller writes independently.
  const uint32_t idle = now - g_progress_ms;
  lv.idle_ms = idle < lv.since_ms ? idle : lv.since_ms;
  lv.interval_ms = g_cycle_interval_ms;
  return lv;
}

RestartNote getRestartNote() {
  captureRestartNote();
  return g_prev_note;
}

void noteSelfHealRestart(SelfHeal reason, uint32_t a, uint32_t b) {
  g_rtc_note.reason = (uint32_t)reason;
  g_rtc_note.uptime_s = millis() / 1000UL;
  g_rtc_note.a = a;
  g_rtc_note.b = b;
  g_rtc_note.magic = kNoteMagic;  // last, so a reset mid-write leaves no half-formed note
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

std::shared_ptr<const transit::Snapshot> snapshotPtr() {
  // The display task's own last-good pointer (ui_lock.h). Touched only when onDisplayTask() is
  // true, so it is per-task state and needs no lock; on a miss the display keeps rendering the
  // Snapshot it rendered last frame, which is a second of staleness on a page that already prints
  // "updated 12 s ago" - not a blank one.
  static LastGood<std::shared_ptr<const transit::Snapshot>> ui_last;
  if (onDisplayTask()) {
    // `outgoing` matters. Overwriting ui_last in place would drop its reference to the PREVIOUS
    // Snapshot inside the critical section, and when that reference is the last one - the poller has
    // already published past it - dropping it runs the whole Snapshot's destructor there. Moving it
    // out first makes the store a bare refcount bump and leaves the free until after the give.
    std::shared_ptr<const transit::Snapshot> outgoing;
    if (takeShared(g_mutex, kSnapshotWaitMs)) {
      outgoing = std::move(ui_last.slot());
      ui_last.slot() = g_snapshot;  // a refcount bump: no allocation, no free, cannot throw
      ui_last.hit();
      giveShared(g_mutex);
    } else {
      ui_last.miss();
    }
    return ui_last.value();  // `outgoing` is released here, with the lock long gone
  }
  std::shared_ptr<const transit::Snapshot> copy;
  if (takeShared(g_mutex, kSnapshotWaitMs)) {
    copy = g_snapshot;
    giveShared(g_mutex);
  }
  return copy;
}

PollStatus getPollStatus() {
  PollStatus copy;
  if (takeShared(g_mutex, kStatusWaitMs)) {
    copy = g_status;
    giveShared(g_mutex);
  }
  return copy;
}

bool tryGetPollStatus(PollStatus *out) {
  // The display task gets a zero wait from takeShared() and therefore cannot block here at all
  // (ui_lock.h); other tasks get kStatusWaitMs. Leaving *out alone on a miss is the point - the
  // device page then redraws the value it last read instead of blanking to "no poll yet" for one
  // tick - so this keeps its out-parameter shape rather than growing a LastGood of its own.
  if (out == nullptr) return false;
  if (!takeShared(g_mutex, kStatusWaitMs)) return false;
  try {
    *out = g_status;  // copies a std::string, so it can throw; the mutex must not be lost with it
  } catch (const std::bad_alloc &) {
    heapTraceMark(kStageOomStatus);  // diag branch
    giveShared(g_mutex);
    throw;  // loop()'s guard in main.cpp skips the frame
  }
  giveShared(g_mutex);
  return true;
}

AlertsStatus getAlertsStatus() {
  AlertsStatus s;
  uint32_t t = g_alerts_fetched_ms;
  if (t != 0) {
    s.fetched = true;
    s.age_s = (millis() - t) / 1000;  // unsigned: correct across the 49-day millis() wrap
  }
  return s;
}

StopSummaryView getStopSummary(const std::string &stop_key) {
  // Per-stop last-good, for the display task only (ui_lock.h). Without it a miss returned
  // has_value = false, which stats_screen.cpp renders as "loading..." with the numbers and the
  // meter hidden - so a single busy lock replaced a whole panel of statistics with a caption and
  // then put it back a second later. StopSummaryView is 40-odd bytes plus the key, and DESIGN.md
  // SS6 caps the stop list at 8, so this is a few hundred bytes for the display task.
  static std::vector<std::pair<std::string, StopSummaryView>> ui_last;
  const bool ui = onDisplayTask();

  StopSummaryView view;
  if (!takeShared(g_mutex, kSummaryWaitMs)) {
    if (ui) {
      for (const auto &e : ui_last) {
        if (e.first == stop_key) return e.second;
      }
    }
    return view;
  }
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
  giveShared(g_mutex);

  if (ui) {
    bool stored = false;
    for (auto &e : ui_last) {
      if (e.first != stop_key) continue;
      e.second = view;
      stored = true;
      break;
    }
    // Only ever grows to the stop list's own size; evictUnconfiguredSummaries() drops stops from
    // the poller's cache, and a stop that is gone simply stops being asked for here.
    if (!stored && ui_last.size() < kMaxStops) ui_last.emplace_back(stop_key, view);
  }
  return view;
}

// ---- Instrument readouts (diag branch, net_poller.h) -----------------------------------------
uint32_t snapshotsLive() { return g_snapshots_live.load(); }
uint32_t failedPolls() { return g_failed_polls; }
uint32_t wedgedPolls() { return g_wedged_polls; }

}  // namespace transit_app
