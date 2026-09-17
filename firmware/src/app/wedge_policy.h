// When a heap that has wedged should be restarted, as pure arithmetic over one cycle's outcome.
//
// WHY THIS IS A FILE OF ITS OWN (0.3.2-rc1). The rule used to be an inline condition in
// pollerTask(), and on 2026-09-17 it did not do what everyone believed it did. The board sat with
// every poll failing at `oom-transit`, largest free block 3,444-4,596 B, `wedged_polls` reading 4
// at 15:50 - and it was still in that state at 16:07, seventeen minutes later, when the owner
// hard-reset it over USB. The expectation was "15 wedged polls at 30 s = a restart by 15:56".
//
// THE TRACE, because the answer is not the obvious one and the obvious one is wrong.
//
//  1. The tally was NOT being reset. `wedged_polls` = 4 was correct: only about four cycles had
//     failed by 15:50. (`failed_polls` = 11 counts every failure since boot, not consecutive ones;
//     the other seven were older and unrelated.)
//  2. `getPollStatus()` returning a default-constructed PollStatus on a lock miss does not break
//     the rule either - PollStatus::ok defaults to FALSE, so a miss reads as "not ok", which is
//     the direction that ADVANCES the tally. Checked, and ruled out.
//  3. The second publish of a cycle (the one that carries newly fetched alerts) copies
//     `last_poll_ok` from the published Snapshot, so it cannot overwrite a failure with a success.
//     Also checked, also ruled out.
//  4. What actually happened is the FAILURE BACKOFF. A failed poll drives
//     `nextIntervalS()`'s exponential backoff, which saturates at `kBaseBackoffS << 3` = 240 s
//     after four consecutive failures. So fifteen consecutive failed polls is
//     30 + 60 + 120 + 12 x 240 = **51 minutes**, not seven and a half. The board was behaving
//     exactly as written; the threshold was written against a cadence that a failing board does
//     not run at. The comment in net_poller.cpp said "which with the failure backoff is several
//     minutes" - an underestimate by a factor of eight.
//  5. The poller-stall net in main.cpp could not help and was not meant to: the poller WAS
//     completing cycles, and that net's window is a multiple of the interval the poller is
//     actually running at, backoff included, so it stretches with the backoff too.
//
// Two conclusions, and they are what this header encodes.
//
// FIRST: count out-of-memory outcomes DIRECTLY, at the catch site, not by comparing the largest
// free block after the fact. A cycle that caught std::bad_alloc is a cycle that could not do its
// job for want of memory - that is a fact the cycle itself knows, and turning it back into an
// inference from a heap reading taken later is how the old rule acquired a second condition that
// could lapse independently. Three consecutive such cycles restart the board, unconditionally.
// With the backoff that is 30 + 60 + 120 = about three and a half minutes, against the 51 the old
// rule really cost.
//
// SECOND: keep the old rule as the slower backstop for a wedge that does NOT throw - a heap so
// cut up that every fetch fails without any single allocation being large enough to raise
// bad_alloc. That one keeps its 15-cycle threshold, because its condition (a failed poll while the
// largest block is small) can also be met by an ordinary SEPTA outage on a board whose heap merely
// happens to be busy, and rebooting for that would be worse than the stale screen.
//
// Pure and host-tested (`pio test -e native -f test_wedge`), the same shape as poller_liveness.h,
// proxy_queue.h, heap_reserve.h and admission.h.
#pragma once
#include <cstddef>
#include <cstdint>

namespace transit_app {

// Why the poller decided to restart. Reported as `wedge_reason` on GET /api/debug/ui, and stored
// in the RTC restart note so GET /api/state's last_restart can name it after the fact.
enum class WedgeReason : uint8_t {
  None = 0,
  OutOfMemory = 1,  // the cycle caught std::bad_alloc
  Starved = 2,      // the cycle reported failure while the largest free block was tiny
};

// Three consecutive out-of-memory cycles. A lossless restart beats a stale display: the arrivals
// are refetched within seconds of boot, and nothing this device holds needs to survive (the stats
// log is on the SD card, the config is on LittleFS, and the RTC note records why).
constexpr uint32_t kOomPollsBeforeReboot = 3;

// ...against fifteen for the non-throwing wedge, which is the old threshold kept deliberately:
// that condition can be met by an ordinary outage and must stay hard to trip.
constexpr uint32_t kStarvedPollsBeforeReboot = 15;

// "Critically small" for the non-throwing rule. A normal idle board sits at 20-30 KB.
constexpr size_t kWedgeLargestBlock = 6 * 1024;

// The two tallies. Separate rather than one, because they mean different things and reach their
// thresholds at very different speeds.
struct WedgeState {
  uint32_t oom_streak = 0;      // consecutive cycles that caught std::bad_alloc
  uint32_t starved_streak = 0;  // consecutive failed cycles with a critically small largest block
};

// One completed cycle's effect on the tallies.
//
// `ota_busy` zeroes BOTH and forgets the count rather than resuming it: an upload takes the heap
// for the length of a ~1.7 MB write, which is precisely the condition these look for, and
// restarting mid-Update.write() throws away the owner's upload at the worst possible moment.
// Whatever the heap was doing before the upload is not evidence about what it is doing after.
//
// `cycle_oom` is the fact the cycle knows about itself - a std::bad_alloc was caught in it - and
// it advances the OOM tally with NO heap comparison attached. `largest_after` is only consulted
// for the non-throwing rule.
constexpr WedgeState nextWedgeState(WedgeState s, bool ota_busy, bool cycle_oom, bool poll_ok,
                                     size_t largest_after) {
  if (ota_busy) return WedgeState{0, 0};
  WedgeState out;
  out.oom_streak = cycle_oom ? s.oom_streak + 1 : 0;
  out.starved_streak =
    (!poll_ok && largest_after < kWedgeLargestBlock) ? s.starved_streak + 1 : 0;
  return out;
}

// Whether to restart now, and which rule said so. The OOM rule is checked first: when both are
// satisfied it is the one that fired, and it is the more specific statement.
constexpr WedgeReason wedgeVerdict(WedgeState s) {
  if (s.oom_streak >= kOomPollsBeforeReboot) return WedgeReason::OutOfMemory;
  if (s.starved_streak >= kStarvedPollsBeforeReboot) return WedgeReason::Starved;
  return WedgeReason::None;
}

}  // namespace transit_app
