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
#include <memory>
#include <string>
#include <vector>

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

// Allocates the poll cycle's shared byte scratch (6 KB, transit_core PollBuffers) and the
// transport's BusSchedules buffer (4 KB), for the same reason and at the same moment as
// preallocateTracker(): both are multi-kilobyte CONTIGUOUS requests, and on this board the largest
// free block - not the free heap - is what runs out (DESIGN.md SS5 "the poll working set",
// SS12.1). One buffer serves the GTFS-RT entity buffer, then every buffered JSON response, then
// the Indego feed scanner, because none of them is live at the same time as another. Returns false
// if the allocation failed, in which case each cycle falls back to per-call buffers exactly as
// before - a poll that is likelier to fail, not one that cannot run.
bool preallocatePollBuffers();

// The shared byte scratch itself, for the one consumer that is not inside transit_core: the Indego
// feed scanner (bike_service.h). Null until preallocatePollBuffers() has succeeded.
std::vector<uint8_t> *pollScratch();

// What the shared scratch has actually been asked to hold (0.3.2-rc1), reported by
// GET /api/debug/ui. The reservation was a guess - kJsonBodyCap is 16 KB and the reservation is
// 6 KB, so a bigger body reallocated the vector and poll-start reallocated it back, every cycle -
// and nothing on this device could say how big a body had ever been. Now it can: if `max_bytes`
// sits above `reserve_bytes`, the reservation in transit_core is the wrong number and should be
// changed in the source rather than discovered again at runtime.
struct ScratchStats {
  uint32_t max_bytes = 0;      // largest response body buffered since boot
  uint32_t reserve_bytes = 0;  // the reservation it is currently held at (ratchets up)
  uint32_t capacity = 0;       // what the vector is holding right now
  uint32_t grows = 0;          // times a body went past the reservation and forced a realloc
};
ScratchStats getScratchStats();

// Roughly how many heap bytes the long-lived structures are holding (0.3.2-rc1), reported by
// GET /api/debug/ui beside the heap figures. The point is attribution: "free8 fell 4 KB over an
// hour" is a fact with no owner until these are beside it, and the schedule and alerts caches are
// the two things on this device that legitimately hold data across cycles and could therefore
// legitimately grow. Lower bounds, not an audit - only bytes that actually came off the heap are
// counted (a std::string of 15 characters or fewer lives inside the object), and allocator headers
// are not. Safe to call from any task; takes the poller's shared lock briefly for the Snapshot.
struct MemorySizes {
  uint32_t sched_cache_bytes = 0;
  uint32_t alerts_cache_bytes = 0;
  uint32_t snapshot_bytes = 0;      // the currently published Snapshot
  uint32_t retained_capacity = 0;   // PollBuffers::retained slots (DESIGN.md SS5)
  uint32_t tv_capacity = 0;         // PollBuffers::tv slots
};
MemorySizes getMemorySizes();

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

// The latest Snapshot as a shared, immutable pointer - the form it is published in (net_poller.cpp
// g_snapshot). Safe from any task, and the right call for a reader that only wants to LOOK at the
// data: taking it is one refcount bump under the lock instead of a whole-Snapshot copy, so it
// neither allocates nor makes anyone else wait out an allocation. Null only before the first
// publish, or when the read was refused and the caller has never had one.
//
// On the LVGL task it cannot block (ui_lock.h takeShared()), and on a refusal it returns the
// pointer that task last held, so the screen redraws last frame's arrivals rather than blanking.
std::shared_ptr<const transit::Snapshot> snapshotPtr();


// Returns a copy of the latest poll diagnostics, safe to call from any task. Waits up to 1 s for
// the poller's mutex on a non-display task. NOT for the LVGL task - see tryGetPollStatus().
PollStatus getPollStatus();

// The same diagnostics for callers that must never wait on the poller's lock: the LVGL task
// (DESIGN.md SS5, and SS12.1's vTaskPriorityDisinheritAfterTimeout assert, which was caused by
// exactly a 1 s wait from this task). On that task the take does not wait at all; on a miss it
// returns false and leaves *out untouched, so the caller keeps showing the value it last read
// rather than blanking the line. `out` must not be null.
bool tryGetPollStatus(PollStatus *out);

// ---------------------------------------------------------------------------------------------
// Liveness: has the poller completed a cycle lately? (DESIGN.md SS12.1)
//
// The heap-wedge self-heal in pollerTask() counts *failed* cycles, which means it can only ever
// see a poller that is still going round. A poller that STOPS - blocked inside pollOnce(), in
// HTTPClient or under it in lwIP - never reaches that check, and the task watchdog does not cover
// it either (CONFIG_ESP_TASK_WDT_PANIC watches IDLE0, and a task blocked on a semaphore or a
// bounded socket read yields, so IDLE0 runs and nothing panics). So the poller stamps itself and
// something on another task - main.cpp's display loop - watches the stamp go stale. A stuck poller
// cannot check itself.
//
// TWO stamps, not one (2026-09-16). "A cycle completed" is too coarse to be the only evidence: a
// legitimate cycle on a blackholing network can run for many minutes (poller_liveness.h has the
// arithmetic), so a window wide enough to cover one would be far too wide to catch a freeze. The
// poller therefore also stamps every fetch it starts, and the net measures the LATER of the two.
// `since_ms` keeps its old meaning - seconds since a cycle completed, which is what
// /api/state.last_poll.since_s reports - and `idle_ms` is what the net judges.
//
// Lock-free on purpose: four aligned 32-bit/bool values written only by the poller task and read
// by anyone. No mutex, because the reader is the display loop and it must never wait on the
// poller's lock (DESIGN.md SS5), and because a torn read - a fresh stamp next to the previous
// cycle's interval - is harmless: both fields only ever shift the verdict by one interval.
struct PollerLiveness {
  bool armed = false;              // startNetPoller() has run. False through setup and the Wi-Fi
                                   // captive portal, when polling is deliberately not happening.
  bool before_first_cycle = true;  // no cycle has finished yet, so the boot grace applies
  uint32_t since_ms = 0;           // millis() since the last COMPLETED cycle, any outcome
  uint32_t idle_ms = 0;            // millis() since the poller last did ANYTHING: completed a
                                   // cycle or started a fetch. Never greater than since_ms.
  uint32_t interval_ms = 30000;    // the interval that cycle picked for the next one (backoff included)
};
PollerLiveness getPollerLiveness();

// Why the previous boot restarted itself, if it did. Both writers are in net_poller.cpp's own
// reboot paths, which is why this lives here.
enum class SelfHeal : uint8_t {
  None = 0,
  HeapWedge = 1,  // pollerTask's consecutive-failed-polls + tiny-largest-block reboot
  PollStall = 2,  // main.cpp's liveness net: the poller did nothing for pollerStallTimeoutMs()
  LvglPool = 3,   // ui/lv_assert_hook.cpp: LVGL's pool ran out and there is no safe way to continue
  HeapOom = 4,    // pollerTask: three consecutive cycles caught std::bad_alloc (wedge_policy.h).
                  // Distinct from HeapWedge on purpose - "the cycle could not get memory" is a
                  // fact the cycle reported, while HeapWedge is an inference from a heap reading
                  // after a failed poll, and the two want telling apart in a restart note.
};
// captureRestartNote() validates the stored value against this; keep it equal to the last entry.
constexpr SelfHeal kSelfHealMax = SelfHeal::HeapOom;

struct RestartNote {
  SelfHeal reason = SelfHeal::None;
  uint32_t uptime_s = 0;  // how long that boot had been up
  uint32_t a = 0;         // HeapWedge/HeapOom: consecutive polls. PollStall: seconds of silence.
                          // LvglPool: LVGL pool bytes free when the assert fired.
  uint32_t b = 0;         // HeapWedge/HeapOom: largest free block, bytes. PollStall: interval, s.
                          // LvglPool: the pool's high-water mark, bytes.
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
//
// On the LVGL task the lock is taken without waiting (ui_lock.h) and a refusal returns the last
// view this task read for THIS STOP, so a busy lock costs a second of staleness rather than
// replacing a panel of statistics with "loading..." and putting it back on the next tick.
StopSummaryView getStopSummary(const std::string &stop_key);

// ---------------------------------------------------------------------------------------------
// Instrument readouts (diag branch, 2026-09-17). All three are reported by GET /api/debug/ui, the
// one endpoint outside refuseIfLowHeap()'s 503 gate, so they can still be read on a board whose
// heap has already gone. None of them allocates or takes a lock.
//
// How many published Snapshots are alive right now. The audit's expected value is 3 during a
// cycle's optional tail - the poller's working copy, the published one and the display task's
// last-good reference, at 4-8 KB each - so a reading that climbs past that is a reference nobody
// is releasing, which is the difference between "fragmented" and "leaking".
uint32_t snapshotsLive();
// Cycles that reported failure since boot, cumulative. On the observed failure loop this climbs by
// one every 30 s once the first fetch has thrown.
uint32_t failedPolls();
// pollerTask's live wedge tally: consecutive failed cycles with a largest block under 6 KB. Reaches
// 15 and the board reboots itself, so this is how far through the ~10 minute loop a sample is.
uint32_t wedgedPolls();

// The OOM tally (wedge_policy.h): consecutive cycles that caught std::bad_alloc. THREE reboots the
// board - about three and a half minutes once the failure backoff has stretched the interval -
// against the fifteen the old rule wanted, which with that same backoff was fifty-one minutes.
// Reported as `oom_streak` on GET /api/debug/ui.
uint32_t oomStreak();

// Which self-heal rule has something to say about the cycle that just ended, as a short word for
// GET /api/debug/ui: "none", "oom" or "starved". It is not a prediction - a streak of one is still
// "oom" - it is what the tallies currently hold.
const char *wedgeReason();

}  // namespace transit_app
