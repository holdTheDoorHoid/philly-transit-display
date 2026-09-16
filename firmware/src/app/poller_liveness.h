// Threshold arithmetic for the poller-liveness net (DESIGN.md SS12.1).
//
// Deliberately pure and Arduino-free: no globals, no millis(), no FreeRTOS. Everything here is a
// function of numbers, which is what lets `pio test -e native` cover it on the host
// (firmware/test/test_liveness/test_main.cpp includes this header directly). net_poller.cpp keeps
// the stamps; main.cpp's display loop does the asking. Header-only and constexpr, so it costs no
// flash beyond the one comparison it inlines into loop().
//
// WHY the numbers below are what they are:
//
//   * The threshold is a multiple of the poller's ACTIVE interval, not a constant. device.poll_seconds
//     is user-settable from 5 to 600 s (DESIGN.md SS6.1) and the failure backoff stretches the
//     interval to 300 s, so a fixed "no poll for two minutes" would reboot a perfectly healthy
//     device that was merely configured to poll slowly, or one that was backing off from a SEPTA
//     outage.
//   * The floor exists because a multiple of a SHORT interval is too tight. One cycle's own worst
//     case is bounded by http_fetch.cpp, not by the interval: three attempts per URL, each with an
//     absolute deadline of 2x the 15 s fetch timeout, plus 0.5 s + 1 s of retry backoff - about
//     93 s for a single trickling URL - and a cycle fetches several. On top of that the poller's
//     idle slices run one queued proxy or stats job between cycles. kStallFloorMs is set above any
//     of that, so a genuinely slow network produces a late cycle, never a reboot.
//   * The boot grace exists because pollerTask waits up to 45 s for NTP before its FIRST cycle
//     (net_poller.cpp), and that cycle then has to run. Until one cycle has completed there is no
//     evidence either way, so the window is widened rather than armed early.
#pragma once
#include <cstdint>

namespace transit_app {

// Whole poll intervals of complete silence before the poller is called stalled.
constexpr uint32_t kStallIntervals = 6;

// Absolute floor, whatever the interval. Five minutes: comfortably above one worst-case cycle
// (see above) and above the one queued idle-slice job that can sit between two cycles, and still
// short enough that a board frozen on the arrivals page recovers itself within a few minutes
// rather than sitting there stale until someone notices and pulls the plug.
constexpr uint32_t kStallFloorMs = 300000;

// Added to the window until the first cycle has completed: the 45 s NTP wait plus the first cycle
// itself plus room for a cold DNS/TLS path.
constexpr uint32_t kStallBootGraceMs = 120000;

// device.poll_seconds' own ceiling (DESIGN.md SS6.1), in ms. Clamped rather than trusted so a
// garbled interval can only ever make the window LONGER, never shorter, and never overflow.
constexpr uint32_t kMaxIntervalMs = 600000;

// How long the poller may go without completing a single cycle - of ANY outcome, success or
// failure - before the display loop is entitled to restart the board.
constexpr uint32_t pollerStallTimeoutMs(uint32_t interval_ms, bool before_first_cycle) {
  const uint32_t interval = interval_ms > kMaxIntervalMs ? kMaxIntervalMs : interval_ms;
  uint32_t window = interval * kStallIntervals;  // <= 3,600,000: no overflow after the clamp
  if (window < kStallFloorMs) window = kStallFloorMs;
  return before_first_cycle ? window + kStallBootGraceMs : window;
}

// `since_ms` is millis()-now minus the stamp the poller writes at the end of every cycle; unsigned
// subtraction makes it correct across the 49-day millis() wrap. Strictly greater than, so a
// hand-computed "exactly the window" case is not a stall.
constexpr bool pollerHasStalled(uint32_t since_ms, uint32_t interval_ms, bool before_first_cycle) {
  return since_ms > pollerStallTimeoutMs(interval_ms, before_first_cycle);
}

}  // namespace transit_app
