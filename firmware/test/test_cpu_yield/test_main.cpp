// Host tests for the poller task's yield budget (DESIGN.md SS12.1, src/app/cpu_yield.h).
//
// `pio test -e native -f test_cpu_yield`. The FreeRTOS half of the header compiles out under
// NATIVE_TEST, the same way ui_lock.h's semaphore half does, leaving the two things worth pinning
// down on the host: that the interval keeps an order of magnitude of headroom against the 5 s task
// watchdog, and that "is a yield due?" stays correct across the 49-day millis() wrap - the one
// arithmetic bug in this header that would show up as a board that stopped yielding after seven
// weeks of uptime and then panicked.
#include <unity.h>

#include "../../src/app/cpu_yield.h"

using transit_app::cpuYieldDue;
using transit_app::kCpuYieldIntervalMs;
using transit_app::kTaskWdtTimeoutMs;

void setUp(void) {}
void tearDown(void) {}

// The budget is what makes the rule safe rather than merely present: a loop that honours it can
// overshoot a slice many times over and still be nowhere near the watchdog.
void test_the_interval_leaves_the_watchdog_real_headroom(void) {
  TEST_ASSERT_EQUAL_UINT32(40, kCpuYieldIntervalMs);
  TEST_ASSERT_EQUAL_UINT32(5000, kTaskWdtTimeoutMs);
  TEST_ASSERT_TRUE(kCpuYieldIntervalMs * 8 < kTaskWdtTimeoutMs);
  TEST_ASSERT_TRUE(kTaskWdtTimeoutMs / kCpuYieldIntervalMs >= 100);
}

void test_a_yield_is_due_only_once_the_interval_has_passed(void) {
  TEST_ASSERT_FALSE(cpuYieldDue(1000, 1000));
  TEST_ASSERT_FALSE(cpuYieldDue(1000, 1039));
  TEST_ASSERT_TRUE(cpuYieldDue(1000, 1040));
  TEST_ASSERT_TRUE(cpuYieldDue(1000, 9000));
}

// millis() wraps at 2^32 ms, about 49.7 days - well inside the uptime of a display on a wall. An
// age computed as a signed difference goes hugely negative there and the loop stops yielding
// exactly once; unsigned arithmetic gives the right answer on both sides of the wrap.
void test_the_millis_wrap_does_not_stop_the_yielding(void) {
  const uint32_t before_wrap = 0xFFFFFFF0u;  // 16 ms of counter left
  TEST_ASSERT_FALSE(cpuYieldDue(before_wrap, before_wrap + 10));  // 10 ms later, wrapped
  TEST_ASSERT_TRUE(cpuYieldDue(before_wrap, before_wrap + 40));   // 40 ms later, wrapped
  TEST_ASSERT_TRUE(cpuYieldDue(0xFFFFFF00u, 0x00000100u));        // 512 ms across the wrap
}

// An explicit interval is honoured, so a future caller with a different budget is not silently
// given the default.
void test_an_explicit_interval_is_used(void) {
  TEST_ASSERT_FALSE(cpuYieldDue(0, 99, 100));
  TEST_ASSERT_TRUE(cpuYieldDue(0, 100, 100));
  TEST_ASSERT_TRUE(cpuYieldDue(0, 1, 1));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_the_interval_leaves_the_watchdog_real_headroom);
  RUN_TEST(test_a_yield_is_due_only_once_the_interval_has_passed);
  RUN_TEST(test_the_millis_wrap_does_not_stop_the_yielding);
  RUN_TEST(test_an_explicit_interval_is_used);
  return UNITY_END();
}
