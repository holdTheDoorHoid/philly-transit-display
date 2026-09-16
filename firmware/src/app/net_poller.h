// FreeRTOS network-polling task, pinned to core 0. DESIGN.md SS5: "Network
// polling runs on core 0. Shared state is a Snapshot guarded by a mutex,
// swapped whole (never mutated in place)."
//
// DESIGN.md SS4.7's whole poll cycle lives here: transit_core's SeptaSource::pollBusStops()/
// pollRailStops() do the fetch+merge orchestration; this file supplies the HttpGet glue
// (http_fetch.cpp), the ScheduleCache, the alerts fetch (5 min, separate from pollBusStops -
// see source.h), the poll-interval/backoff state machine, and feeds every StopSnapshot to
// transit_stats::ArrivalTracker + sd_logger (DESIGN.md SS9.1).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "transit_core/model.h"
#include "transit_stats/summary.h"

namespace transit_app {

// Poller-specific diagnostics: richer than what lives on transit::Snapshot,
// used for GET /api/state's "last_poll" field (DESIGN.md SS7) and the
// device-info screen.
struct PollStatus {
  bool has_polled = false;  // false until the first attempt completes
  bool ok = false;
  uint32_t last_poll_epoch = 0;
  int last_http_status = 0;
  size_t last_bytes = 0;
  std::string last_error;
};

// Starts the polling task: reads the active config (config_store::getActiveConfig()) once per
// cycle, so a PUT /api/config takes effect on the very next poll without a restart. `poll_seconds`
// is only the *initial* cadence (also DeviceConfig::poll_seconds's default) - the task re-reads
// the live config every cycle and re-derives the interval per DESIGN.md SS4.7 (15s when any
// arrival is under 3 min out, exponential backoff on failure capped at 5 min). Must be called
// once, after Wi-Fi is connected.
void startNetPoller(uint32_t poll_seconds);

// Allocates the ~16 KB ArrivalTracker while the heap is still unfragmented. Call early in
// setup(), before Wi-Fi. Returns false (and logging stays disabled) if the allocation failed.
bool preallocateTracker();

// Creates the poller task (12 KB stack) and its primitives without starting to poll. Call early
// in setup(), before Wi-Fi, for the same heap-fragmentation reason as preallocateTracker().
void initNetPoller();

// Wakes the poller task immediately instead of waiting out its current interval. Wired to
// web_server.cpp's onConfigChanged hook (DESIGN.md SS7: "PUT /api/config ... triggers immediate
// re-poll") - also invalidates the BusSchedules cache, since a config change may have added a
// stop whose schedule was never fetched. Safe to call before startNetPoller() (a no-op then).
// Wakes the poller now. `data_changed` (stops, weather or bike settings differ) also drops the
// schedule, weather and bike caches; a brightness or theme change must not re-download 400 KB of
// bike feed.
void requestRepoll(bool data_changed = true);

// Returns a copy of the latest Snapshot, safe to call from any task.
transit::Snapshot getSnapshot();

// Returns a copy of the latest poll diagnostics, safe to call from any task.
PollStatus getPollStatus();

// ---------------------------------------------------------------------------------------------
// Liveness: has the poller completed a cycle lately? (DESIGN.md SS12.1)
//
// The heap-wedge self-heal in pollerTask() counts *failed* cycles, which means it can only ever
// see a poller that is still going round. A poller that STOPS - blocked inside pollOnce(), in
// HTTPClient or under it in lwIP - never reaches that check, and the task watchdog does not cover
// it either (CONFIG_ESP_TASK_WDT_PANIC watches IDLE0, and a task blocked on a semaphore or a
// bounded socket read yields, so IDLE0 runs and nothing panics). So the poller is stamped here at
// the end of EVERY cycle whatever the outcome, and something on another task - main.cpp's display
// loop - watches the stamp go stale. A stuck poller cannot check itself.
//
// Lock-free on purpose: three aligned 32-bit/bool values written only by the poller task and read
// by anyone. No mutex, because the reader is the display loop and it must never wait on the
// poller's lock (DESIGN.md SS5), and because a torn read - a fresh stamp next to the previous
// cycle's interval - is harmless: both fields only ever shift the verdict by one interval.
struct PollerLiveness {
  bool armed = false;              // startNetPoller() has run. False through setup and the Wi-Fi
                                   // captive portal, when polling is deliberately not happening.
  bool before_first_cycle = true;  // no cycle has finished yet, so the boot grace applies
  uint32_t since_ms = 0;           // millis() since the last COMPLETED cycle, any outcome
  uint32_t interval_ms = 30000;    // the interval that cycle picked for the next one (backoff included)
};
PollerLiveness getPollerLiveness();

// Why the previous boot restarted itself, if it did. Both writers are in net_poller.cpp's own
// reboot paths, which is why this lives here.
enum class SelfHeal : uint8_t {
  None = 0,
  HeapWedge = 1,  // pollerTask's consecutive-failed-polls + tiny-largest-block reboot
  PollStall = 2,  // main.cpp's liveness net: no cycle completed for pollerStallTimeoutMs()
};

struct RestartNote {
  SelfHeal reason = SelfHeal::None;
  uint32_t uptime_s = 0;  // how long that boot had been up
  uint32_t a = 0;         // HeapWedge: consecutive failed polls. PollStall: seconds of silence.
  uint32_t b = 0;         // HeapWedge: largest free block, bytes. PollStall: active interval, s.
};

// What the PREVIOUS boot recorded before restarting itself. Kept in RTC memory, which survives
// ESP.restart() but not a power cycle, and cleared the first time it is read so it is reported for
// exactly one boot. GET /api/state surfaces it (DESIGN.md SS7).
RestartNote getRestartNote();

// Records why THIS boot is about to restart itself. Call immediately before ESP.restart().
void noteSelfHealRestart(SelfHeal reason, uint32_t a, uint32_t b);

// When the service-alert feeds were last fetched (DESIGN.md SS4.7: 5 min cadence, only with
// config.alerts), for the device page's "Data sources" card. fetched=false until the first fetch
// and again after alerts are switched off. Lock-free: one aligned 32-bit millis() stamp.
struct AlertsStatus {
  bool fetched = false;
  uint32_t age_s = 0;
};
AlertsStatus getAlertsStatus();

// What getStopSummary() can tell a caller without going anywhere near the SD card (F27).
struct StopSummaryView {
  // false = nothing has been computed for this stop yet. The caller must render that as "loading"
  // or "no data yet", NEVER as `summary`'s zeroes: "0% on time, n=0" is a specific, wrong claim.
  bool has_value = false;
  bool pending = false;   // a refresh is queued; the poller will do it in its next idle slice
  uint32_t age_s = 0;     // seconds since `summary` was computed (0 when !has_value)
  // DESIGN.md SS9.2: how many of `summary.samples` carry an inference marker. Every `arrive` row
  // in this log is an inference; this is the count that says which method was recorded, and the
  // UI shows it so a derived arrival is never presented as a measured one.
  uint32_t inferred = 0;
  transit_stats::StopSummary summary;
};

// The current per-stop stats summary (DESIGN.md SS8 "Stats page") for `stop_key`.
//
// NON-BLOCKING (F27), and that is the point. It used to stream a month of SD log per stop
// synchronously, on whichever task called it - which is ui/stats_screen.cpp on the LVGL task, so
// opening the stats page with several stops configured froze touch and the clock for as long as
// the card took. Now it returns what is cached, says how old that is and whether anything is
// cached at all, and registers a refresh; the poller task does the scanning in its idle slices,
// one stop per slice, at most every 10 minutes per stop. Safe to call from any task, as often as
// the UI likes.
StopSummaryView getStopSummary(const std::string &stop_key);

}  // namespace transit_app
