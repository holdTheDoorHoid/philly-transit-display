// Threshold arithmetic for the poller-liveness net (DESIGN.md SS12.1).
//
// Deliberately pure and Arduino-free: no globals, no millis(), no FreeRTOS. Everything here is a
// function of numbers, which is what lets `pio test -e native` cover it on the host
// (firmware/test/test_liveness/test_main.cpp includes this header directly). net_poller.cpp keeps
// the stamps; main.cpp's display loop does the asking. Header-only and constexpr, so it costs no
// flash beyond the one comparison it inlines into loop().
//
// WHAT THE WINDOW HAS TO COVER - corrected 2026-09-16, and the correction matters.
//
// This header used to say the floor was "comfortably above one worst-case cycle", citing ~93 s for
// a trickling URL. That was a PER-URL figure being used as a per-cycle one, and a cycle fetches
// one URL per stop and more. The real per-cycle cost on a network that SILENTLY DROPS packets (an
// ISP outage with DHCP still up, a captive portal, heavy loss - a refusing network fails in
// milliseconds and never gets near this) was, before the fixes below:
//
//   one URL        3 attempts x (DNS + 4 s connect) + 1.5 s backoff    ~34 s (one DNS server)
//   one schedule   fetchPlausibleSchedule x3, each through the         ~414 s   <- 12 URLs
//                  BusSchedules wrapper's own x4 (net_poller.cpp)
//   one stop       that, plus one TransitView URL                      ~450 s
//   four stops     config_store.h kMaxStops (4 since 0.3.2-rc2), and the poller polls
//                  cfg.stops, not the visible subset                   ~60 MINUTES
//
// No window derived from the stop count can both cover that and still restart a genuinely frozen
// board in time to be useful, so the net was changed to measure a different thing instead:
//
//   1. net_poller.cpp stamps liveness at EVERY fetch the poller makes, not only at the end of a
//      cycle. "The poller is still going round" is what this net is for, and a fetch in flight is
//      evidence of it. The window therefore has to cover ONE fetch, not one cycle - and one fetch
//      IS bounded, by kFetch* below.
//   2. The three retry layers no longer multiply on a transport failure. http_fetch.cpp already
//      spends three attempts and its backoff on a URL; septa_source.cpp's fetchPlausibleSchedule
//      and net_poller.cpp's BusSchedules wrapper now retry only what they exist to retry (a wrong
//      service day, a SEPTA error body - both of which need a real reply), so a dead network costs
//      one URL per fetch instead of twelve.
//
// WHY THE NUMBERS BELOW ARE WHAT THEY ARE:
//
//   * The threshold is a multiple of the poller's ACTIVE interval, not a constant. device.poll_seconds
//     is user-settable from 5 to 600 s (DESIGN.md SS6.1) and the failure backoff stretches the
//     interval to 300 s, so a fixed "no poll for two minutes" would reboot a perfectly healthy
//     device that was merely configured to poll slowly, or one that was backing off from a SEPTA
//     outage.
//   * The floor exists because a multiple of a SHORT interval is too tight: it has to clear one
//     worst-case fetch (oneFetchWorstCaseMs() below) with real margin, and to clear the one queued
//     proxy or stats job the poller's idle slices run between cycles.
//   * The boot grace exists because pollerTask waits up to 45 s for NTP before its FIRST cycle
//     (net_poller.cpp), and that cycle then has to run. Until one cycle has completed there is no
//     evidence either way, so the window is widened rather than armed early.
#pragma once
#include <cstdint>

namespace transit_app {

// ---- What bounds ONE fetch (http_fetch.cpp, plus lwIP underneath it) ---------------------------
// These mirror constants that live in http_fetch.cpp; they are restated here because this is the
// file that has to reason about them and because that reasoning is what the host tests check. If
// http_fetch.cpp's numbers move, these move with them.

// http_fetch.cpp kMaxAttempts.
constexpr uint32_t kFetchAttempts = 3;

// http_fetch.cpp kStreamReadTimeoutMs, which is handed to setConnectTimeout() as well as to
// setTimeout(). On a blackholing network the TCP connect is what burns it.
constexpr uint32_t kFetchConnectTimeoutMs = 4000;

// http_fetch.cpp kBackoffBaseMs: 500 before attempt 2, 1000 before attempt 3.
constexpr uint32_t kFetchBackoffTotalMs = 1500;

// DNS is NOT bounded by anything this firmware sets, and that is worth stating plainly rather than
// leaving as a gap in the arithmetic. HTTPClient::connect() calls NetworkClient::connect(host,...),
// which resolves with Network.hostByName() BEFORE the connect timeout is applied to the socket;
// hostByName takes no timeout argument at all (Arduino-ESP32 3.2.1, Network/src/NetworkClient.cpp).
// What bounds it is lwIP's own DNS retry schedule: DNS_MAX_RETRIES 4 on a 1 s timer with 1/1/2/3 s
// between sends is ~7 s per configured server, and DNS_MAX_SERVERS is 3 (esp32 sdkconfig), so ~21 s
// if all three are configured and all three blackhole. That is the number used here. It is read off
// lwIP's configuration, NOT measured on the board - if a device is ever seen taking longer than
// this to give up on a name, this constant is the thing to re-derive.
constexpr uint32_t kDnsWorstCaseMs = 21000;

// The longest one transit_app::getEx() call can take before it gives up, and therefore the longest
// legitimate gap between two liveness stamps.
constexpr uint32_t oneFetchWorstCaseMs() {
  return kFetchAttempts * (kDnsWorstCaseMs + kFetchConnectTimeoutMs) + kFetchBackoffTotalMs;
}

// ---- The window --------------------------------------------------------------------------------

// Whole poll intervals of complete silence before the poller is called stalled.
constexpr uint32_t kStallIntervals = 6;

// Absolute floor, whatever the interval. Five minutes: 3.9x one worst-case fetch
// (oneFetchWorstCaseMs() = 76.5 s), above the one queued idle-slice job that can sit between two
// cycles, and still short enough that a board frozen on the arrivals page recovers itself within a
// few minutes rather than sitting there stale until someone notices and pulls the plug.
// stallFloorClearsOneFetch() below is the assertion, and the host tests check it.
constexpr uint32_t kStallFloorMs = 300000;

// How much margin the floor is required to have over one worst-case fetch. Three, not one: the
// stamp is written when a fetch STARTS, so a gap of exactly one fetch is normal, and the floor has
// to sit far enough above normal that ordinary variation never reaches it.
constexpr uint32_t kStallFetchMargin = 3;

// Compile-time (and host-test) check that the floor still clears a fetch. If a future change to
// http_fetch.cpp's retry policy breaks this, it breaks here rather than on someone's wall.
constexpr bool stallFloorClearsOneFetch() {
  return kStallFloorMs >= kStallFetchMargin * oneFetchWorstCaseMs();
}
static_assert(stallFloorClearsOneFetch(), "kStallFloorMs no longer clears one worst-case fetch");

// Added to the window until the first cycle has completed: the 45 s NTP wait plus the first cycle
// itself plus room for a cold DNS/TLS path.
constexpr uint32_t kStallBootGraceMs = 120000;

// device.poll_seconds' own ceiling (DESIGN.md SS6.1), in ms. Clamped rather than trusted so a
// garbled interval can only ever make the window LONGER, never shorter, and never overflow.
constexpr uint32_t kMaxIntervalMs = 600000;

// How long the poller may go without doing ANYTHING - completing a cycle of any outcome, or
// starting a fetch - before the display loop is entitled to restart the board.
constexpr uint32_t pollerStallTimeoutMs(uint32_t interval_ms, bool before_first_cycle) {
  const uint32_t interval = interval_ms > kMaxIntervalMs ? kMaxIntervalMs : interval_ms;
  uint32_t window = interval * kStallIntervals;  // <= 3,600,000: no overflow after the clamp
  if (window < kStallFloorMs) window = kStallFloorMs;
  return before_first_cycle ? window + kStallBootGraceMs : window;
}

// `idle_ms` is millis()-now minus the LATER of the two stamps the poller writes: the end of every
// cycle, and the start of every fetch (net_poller.cpp). Unsigned subtraction makes it correct
// across the 49-day millis() wrap. Strictly greater than, so a hand-computed "exactly the window"
// case is not a stall.
constexpr bool pollerHasStalled(uint32_t idle_ms, uint32_t interval_ms, bool before_first_cycle) {
  return idle_ms > pollerStallTimeoutMs(interval_ms, before_first_cycle);
}

}  // namespace transit_app
