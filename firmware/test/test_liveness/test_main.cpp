// Host tests for the poller-liveness threshold arithmetic (DESIGN.md SS12.1).
//
// `pio test -e native -f test_liveness`. The header under test is deliberately pure - no Arduino,
// no FreeRTOS, no globals - so it can be included straight from src/ and exercised here; that is
// the whole reason the numbers were extracted out of an inline expression in main.cpp's loop().
// The relative include is the only thing tying the two together; the native env does not build
// src/, and this header needs nothing built.
#include <unity.h>

#include "../../src/app/poller_liveness.h"

using transit_app::kDnsWorstCaseMs;
using transit_app::kFetchAttempts;
using transit_app::kFetchBackoffTotalMs;
using transit_app::kFetchConnectTimeoutMs;
using transit_app::kMaxIntervalMs;
using transit_app::kStallBootGraceMs;
using transit_app::kStallFetchMargin;
using transit_app::kStallFloorMs;
using transit_app::kStallIntervals;
using transit_app::oneFetchWorstCaseMs;
using transit_app::pollerHasStalled;
using transit_app::pollerStallTimeoutMs;
using transit_app::stallFloorClearsOneFetch;

void setUp(void) {}
void tearDown(void) {}

// The default cadence (device.poll_seconds = 30) is short enough that six intervals is under the
// floor, so the floor is what decides - 5 minutes, not 3.
void test_default_interval_uses_the_floor(void) {
  TEST_ASSERT_EQUAL_UINT32(kStallFloorMs, pollerStallTimeoutMs(30000, false));
  TEST_ASSERT_EQUAL_UINT32(300000, pollerStallTimeoutMs(30000, false));
}

// The 15 s urgent cadence (an arrival under 3 min out) must not shorten the window either.
void test_urgent_interval_uses_the_floor(void) {
  TEST_ASSERT_EQUAL_UINT32(kStallFloorMs, pollerStallTimeoutMs(15000, false));
}

// The shortest configurable interval (5 s) still gets the full floor; this is the case a naive
// "six intervals" would have turned into a 30 s hair trigger.
void test_minimum_configurable_interval_uses_the_floor(void) {
  TEST_ASSERT_EQUAL_UINT32(kStallFloorMs, pollerStallTimeoutMs(5000, false));
}

// Above the crossover the multiple takes over, so a user who configures a slow poll gets a
// proportionally slow window instead of being rebooted by a constant.
void test_long_interval_scales_with_the_multiple(void) {
  // 120 s * 6 = 720 s, comfortably past the 300 s floor.
  TEST_ASSERT_EQUAL_UINT32(720000, pollerStallTimeoutMs(120000, false));
  // device.poll_seconds' ceiling, 600 s (DESIGN.md SS6.1): 3600 s.
  TEST_ASSERT_EQUAL_UINT32(3600000, pollerStallTimeoutMs(600000, false));
  // The failure backoff's ceiling, 300 s (DESIGN.md SS4.7): 1800 s. A device backing off from a
  // SEPTA outage completes a cycle every 300 s, well inside this.
  TEST_ASSERT_EQUAL_UINT32(1800000, pollerStallTimeoutMs(300000, false));
}

// The floor/multiple crossover is exactly where six intervals equals the floor.
void test_crossover_is_where_the_multiple_meets_the_floor(void) {
  const uint32_t crossover_ms = kStallFloorMs / kStallIntervals;  // 50 s
  TEST_ASSERT_EQUAL_UINT32(kStallFloorMs, pollerStallTimeoutMs(crossover_ms, false));
  TEST_ASSERT_EQUAL_UINT32(kStallFloorMs, pollerStallTimeoutMs(crossover_ms - 1000, false));
  TEST_ASSERT_TRUE(pollerStallTimeoutMs(crossover_ms + 1000, false) > kStallFloorMs);
}

// Before the first cycle the window is wider by the boot grace: pollerTask waits up to 45 s for
// NTP and then still has to run a cycle.
void test_boot_grace_widens_the_window(void) {
  TEST_ASSERT_EQUAL_UINT32(kStallFloorMs + kStallBootGraceMs, pollerStallTimeoutMs(30000, true));
  TEST_ASSERT_EQUAL_UINT32(420000, pollerStallTimeoutMs(30000, true));
  TEST_ASSERT_EQUAL_UINT32(3600000 + kStallBootGraceMs, pollerStallTimeoutMs(600000, true));
}

// A nonsense interval may only ever make the window longer, never shorter, and must not overflow.
void test_absurd_interval_is_clamped_not_wrapped(void) {
  const uint32_t clamped = pollerStallTimeoutMs(kMaxIntervalMs, false);
  TEST_ASSERT_EQUAL_UINT32(clamped, pollerStallTimeoutMs(kMaxIntervalMs + 1, false));
  TEST_ASSERT_EQUAL_UINT32(clamped, pollerStallTimeoutMs(0xFFFFFFFFu, false));
  // The bare multiply would have wrapped 0xFFFFFFFF * 6 to something far below the floor.
  TEST_ASSERT_TRUE(pollerStallTimeoutMs(0xFFFFFFFFu, false) >= kStallFloorMs);
}

// An interval of zero (nothing should produce one, but the poller publishes it lock-free and a
// torn read is possible) falls back to the floor rather than to "stalled immediately".
void test_zero_interval_falls_back_to_the_floor(void) {
  TEST_ASSERT_EQUAL_UINT32(kStallFloorMs, pollerStallTimeoutMs(0, false));
}

// The verdict itself: strictly greater than the window, so a silence of exactly the window is not
// yet a stall.
void test_stall_verdict_boundaries(void) {
  TEST_ASSERT_FALSE(pollerHasStalled(0, 30000, false));
  TEST_ASSERT_FALSE(pollerHasStalled(kStallFloorMs - 1, 30000, false));
  TEST_ASSERT_FALSE(pollerHasStalled(kStallFloorMs, 30000, false));
  TEST_ASSERT_TRUE(pollerHasStalled(kStallFloorMs + 1, 30000, false));
}

// The scenario this whole net exists for (DESIGN.md SS12.1): heartbeat stopped at t=68 s on the
// default cadence. At 5 minutes of silence the display loop restarts the board; a minute in, it
// does not.
void test_observed_stall_is_caught_and_a_slow_cycle_is_not(void) {
  TEST_ASSERT_FALSE(pollerHasStalled(60000, 30000, false));    // one slow minute: no
  TEST_ASSERT_FALSE(pollerHasStalled(180000, 30000, false));   // three: still no
  TEST_ASSERT_TRUE(pollerHasStalled(301000, 30000, false));    // five: yes
  // The same silence during the boot window is not yet a verdict.
  TEST_ASSERT_FALSE(pollerHasStalled(301000, 30000, true));
  TEST_ASSERT_TRUE(pollerHasStalled(421000, 30000, true));
}

// ---- What the window actually has to cover (the 2026-09-16 correction) -------------------------
//
// The old version of this test asserted the window cleared "one worst-case URL" and stopped there,
// which is exactly the mistake: a poll cycle is many URLs, and on a blackholing network a cycle
// could outlast the window and reboot a healthy device mid-cycle, repeatedly. The net now stamps
// every FETCH as well as every cycle, so the thing the window must cover is one fetch - and these
// tests pin that arithmetic to http_fetch.cpp's own constants rather than to a remembered number.

// One fetch, spelled out from the parts. http_fetch.cpp: kMaxAttempts 3, kStreamReadTimeoutMs 4 s
// used as the connect timeout, kBackoffBaseMs 500 giving 0.5 s + 1 s; plus lwIP's unbounded-by-us
// DNS wait in front of each attempt.
void test_one_fetch_worst_case_is_the_sum_of_its_parts(void) {
  TEST_ASSERT_EQUAL_UINT32(kFetchAttempts * (kDnsWorstCaseMs + kFetchConnectTimeoutMs) + kFetchBackoffTotalMs,
                           oneFetchWorstCaseMs());
  TEST_ASSERT_EQUAL_UINT32(76500, oneFetchWorstCaseMs());  // 3 * (21 + 4) s + 1.5 s
}

// DNS is in the sum on purpose: hostByName() runs before setConnectTimeout() applies and takes no
// timeout of its own, so leaving it out understates a blackholed fetch by a factor of six.
void test_dns_dominates_one_fetch(void) {
  const uint32_t without_dns = kFetchAttempts * kFetchConnectTimeoutMs + kFetchBackoffTotalMs;
  TEST_ASSERT_EQUAL_UINT32(13500, without_dns);  // the figure the pre-release review derived
  TEST_ASSERT_TRUE(oneFetchWorstCaseMs() > 5 * without_dns);
}

// The floor's whole job. If a change to http_fetch.cpp's retry policy ever breaks this, it breaks
// here rather than on someone's wall. (The header also static_asserts it.)
void test_floor_clears_one_fetch_with_margin(void) {
  TEST_ASSERT_TRUE(stallFloorClearsOneFetch());
  TEST_ASSERT_TRUE(kStallFloorMs >= kStallFetchMargin * oneFetchWorstCaseMs());
  TEST_ASSERT_EQUAL_UINT32(3, kStallFetchMargin);
  // At the shortest and longest cadences alike, since the floor is what decides below the
  // crossover and the multiple only ever makes the window longer.
  TEST_ASSERT_TRUE(pollerStallTimeoutMs(5000, false) > oneFetchWorstCaseMs());
  TEST_ASSERT_TRUE(pollerStallTimeoutMs(600000, false) > oneFetchWorstCaseMs());
}

// The regression this whole change is for, stated in the terms of the bug report: a poll cycle on
// a blackholing network takes far longer than the window, and that MUST NOT be a restart. With the
// progress stamp the clock is `idle_ms`, which never exceeds one fetch while the poller is
// fetching - so a cycle of any length is survivable, at any supported stop count.
void test_a_multi_minute_cycle_is_not_a_stall_while_fetches_are_still_starting(void) {
  // EIGHT stops' worth of fetches, deliberately more than config_store.h's kMaxStops now allows
  // (4 since 0.3.2-rc2): the property under test is "a cycle of any length is survivable", so the
  // test should stay at the harsher number rather than get easier every time the cap comes down.
  // Two fetches each after the retry-stacking fix, all blackholed: the cycle runs for over 20
  // minutes and completes none of it.
  const uint32_t cycle_ms = 8u * 2u * oneFetchWorstCaseMs() + oneFetchWorstCaseMs();
  TEST_ASSERT_TRUE(cycle_ms > pollerStallTimeoutMs(30000, false));   // the cycle DOES outlast the window
  TEST_ASSERT_TRUE(cycle_ms > pollerStallTimeoutMs(30000, true));    // and the boot grace too
  // ...and none of that is a stall, because idle_ms is reset by every fetch that starts.
  for (uint32_t idle = 0; idle <= oneFetchWorstCaseMs(); idle += 1000) {
    TEST_ASSERT_FALSE(pollerHasStalled(idle, 30000, false));
  }
}

// A poller that has genuinely stopped is still caught, and within the same few minutes as before -
// which is the property a stop-count-derived window would have thrown away (even four stops would
// have needed a window of many minutes).
void test_a_frozen_poller_is_still_caught_within_the_floor(void) {
  TEST_ASSERT_TRUE(pollerHasStalled(kStallFloorMs + 1, 30000, false));
  TEST_ASSERT_EQUAL_UINT32(300000, pollerStallTimeoutMs(30000, false));
  TEST_ASSERT_TRUE(pollerStallTimeoutMs(30000, false) < 10u * 60u * 1000u);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_default_interval_uses_the_floor);
  RUN_TEST(test_urgent_interval_uses_the_floor);
  RUN_TEST(test_minimum_configurable_interval_uses_the_floor);
  RUN_TEST(test_long_interval_scales_with_the_multiple);
  RUN_TEST(test_crossover_is_where_the_multiple_meets_the_floor);
  RUN_TEST(test_boot_grace_widens_the_window);
  RUN_TEST(test_absurd_interval_is_clamped_not_wrapped);
  RUN_TEST(test_zero_interval_falls_back_to_the_floor);
  RUN_TEST(test_stall_verdict_boundaries);
  RUN_TEST(test_observed_stall_is_caught_and_a_slow_cycle_is_not);
  RUN_TEST(test_one_fetch_worst_case_is_the_sum_of_its_parts);
  RUN_TEST(test_dns_dominates_one_fetch);
  RUN_TEST(test_floor_clears_one_fetch_with_margin);
  RUN_TEST(test_a_multi_minute_cycle_is_not_a_stall_while_fetches_are_still_starting);
  RUN_TEST(test_a_frozen_poller_is_still_caught_within_the_floor);
  return UNITY_END();
}
