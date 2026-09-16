// Host tests for the poller-liveness threshold arithmetic (DESIGN.md SS12.1).
//
// `pio test -e native -f test_liveness`. The header under test is deliberately pure - no Arduino,
// no FreeRTOS, no globals - so it can be included straight from src/ and exercised here; that is
// the whole reason the numbers were extracted out of an inline expression in main.cpp's loop().
// The relative include is the only thing tying the two together; the native env does not build
// src/, and this header needs nothing built.
#include <unity.h>

#include "../../src/app/poller_liveness.h"

using transit_app::kMaxIntervalMs;
using transit_app::kStallBootGraceMs;
using transit_app::kStallFloorMs;
using transit_app::kStallIntervals;
using transit_app::pollerHasStalled;
using transit_app::pollerStallTimeoutMs;

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

// A whole cycle's own worst case has to fit inside the window, or a slow network would reboot a
// healthy board. http_fetch.cpp: 3 attempts per URL, each with an absolute deadline of 2x the 15 s
// fetch timeout, plus 0.5 s + 1 s of retry backoff.
void test_window_clears_one_worst_case_url(void) {
  const uint32_t worst_url_ms = 3 * (2 * 15000) + 500 + 1000;  // ~91.5 s
  TEST_ASSERT_TRUE(pollerStallTimeoutMs(30000, false) > worst_url_ms + 30000);
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
  RUN_TEST(test_window_clears_one_worst_case_url);
  return UNITY_END();
}
