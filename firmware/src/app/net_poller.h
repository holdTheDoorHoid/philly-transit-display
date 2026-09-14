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
void requestRepoll();

// Returns a copy of the latest Snapshot, safe to call from any task.
transit::Snapshot getSnapshot();

// Returns a copy of the latest poll diagnostics, safe to call from any task.
PollStatus getPollStatus();

// Returns the current per-stop stats summary (DESIGN.md SS8 "Stats page") for `stop_key`, or
// false if that stop has never been observed (never configured, or evicted - see
// transit_stats::ArrivalTracker). Safe to call from any task (UI, web server).
bool getStopSummary(const std::string &stop_key, transit_stats::StopSummary &out);

}  // namespace transit_app
