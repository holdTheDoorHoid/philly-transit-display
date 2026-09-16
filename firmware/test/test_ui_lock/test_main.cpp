// Host tests for the display task's lock policy and its last-good fallback (DESIGN.md SS5, SS12.1).
//
// `pio test -e native -f test_ui_lock`. The pure half of src/app/ui_lock.h is deliberately free of
// Arduino and FreeRTOS - NATIVE_TEST compiles the semaphore half out - so the two things worth
// pinning down can be exercised on the host: that the display task's budget really is zero (a
// "short" wait still times out, and the assert this whole pass exists to remove is reachable only
// from a take that times out), and that a miss hands back the previous value rather than a
// default-constructed one.
#include <unity.h>

#include <string>

#include "../../src/app/ui_lock.h"

using transit_app::kUiLockWaitMs;
using transit_app::LastGood;
using transit_app::lockWaitMsFor;

void setUp(void) {}
void tearDown(void) {}

// The whole point of the policy: the display task does not get a short wait, it gets no wait.
// A short wait still blocks, still times out, and a take that times out is the one path on which
// vTaskPriorityDisinheritAfterTimeout() is reached at all (ui_lock.h).
void test_display_task_waits_zero(void) {
  TEST_ASSERT_EQUAL_UINT32(0, kUiLockWaitMs);
  TEST_ASSERT_EQUAL_UINT32(0, lockWaitMsFor(true, 1000));
  TEST_ASSERT_EQUAL_UINT32(0, lockWaitMsFor(true, 500));
  TEST_ASSERT_EQUAL_UINT32(0, lockWaitMsFor(true, 50));
  TEST_ASSERT_EQUAL_UINT32(0, lockWaitMsFor(true, 0));
}

// Every other task keeps the wait its accessor asked for: the web task answering one request should
// wait for a consistent answer rather than serve a stale field.
void test_other_tasks_keep_their_wait(void) {
  TEST_ASSERT_EQUAL_UINT32(1000, lockWaitMsFor(false, 1000));
  TEST_ASSERT_EQUAL_UINT32(500, lockWaitMsFor(false, 500));
  TEST_ASSERT_EQUAL_UINT32(200, lockWaitMsFor(false, 200));
  TEST_ASSERT_EQUAL_UINT32(50, lockWaitMsFor(false, 50));
}

// Before the first successful read there is nothing to fall back on, and the value is whatever a
// default T is - which is exactly what the accessors used to return on a timeout. have() is what
// tells the two apart.
void test_last_good_starts_empty(void) {
  LastGood<std::string> lg;
  TEST_ASSERT_FALSE(lg.have());
  TEST_ASSERT_EQUAL_UINT32(0, lg.misses());
  TEST_ASSERT_TRUE(lg.value().empty());
}

// A miss after a hit returns the hit's value, not a default. This is the behaviour that turns a
// busy lock into one frame of staleness instead of a blank panel.
void test_miss_returns_the_last_value(void) {
  LastGood<std::string> lg;
  lg.slot() = "69 clear";
  lg.hit();
  TEST_ASSERT_TRUE(lg.have());
  TEST_ASSERT_EQUAL_STRING("69 clear", lg.value().c_str());

  lg.miss();
  TEST_ASSERT_EQUAL_UINT32(1, lg.misses());
  TEST_ASSERT_EQUAL_STRING("69 clear", lg.value().c_str());
  lg.miss();
  lg.miss();
  TEST_ASSERT_EQUAL_UINT32(3, lg.misses());
  TEST_ASSERT_EQUAL_STRING("69 clear", lg.value().c_str());
  TEST_ASSERT_TRUE(lg.have());
}

// A hit clears the run of misses, so the counter reports CONSECUTIVE misses at the moment it is
// read. A total would say "this has missed 400 times since boot" for a lock that is fine; a
// consecutive run is what distinguishes a busy moment from a lock nobody is ever giving back.
void test_hit_resets_the_miss_run(void) {
  LastGood<int> lg;
  lg.slot() = 7;
  lg.hit();
  lg.miss();
  lg.miss();
  TEST_ASSERT_EQUAL_UINT32(2, lg.misses());
  lg.slot() = 9;
  lg.hit();
  TEST_ASSERT_EQUAL_UINT32(0, lg.misses());
  TEST_ASSERT_EQUAL_INT(9, lg.value());
}

// slot() and hit() are two steps because the copy into the slot can throw (the real slots hold
// vectors and strings). A write that never reaches hit() must not be reported as a good value.
void test_partial_write_is_not_a_hit(void) {
  LastGood<std::string> lg;
  lg.slot() = "half written";
  TEST_ASSERT_FALSE(lg.have());
  lg.miss();
  TEST_ASSERT_FALSE(lg.have());
}

// The counter saturates rather than wrapping: a lock that is never given back would otherwise roll
// the evidence of that back to 0 and read as healthy.
void test_miss_counter_saturates(void) {
  LastGood<int> lg;
  for (int i = 0; i < 5; i++) lg.miss();
  TEST_ASSERT_EQUAL_UINT32(5, lg.misses());
  for (uint32_t i = 5; i < 70000; i++) lg.miss();
  TEST_ASSERT_EQUAL_UINT32(70000, lg.misses());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_display_task_waits_zero);
  RUN_TEST(test_other_tasks_keep_their_wait);
  RUN_TEST(test_last_good_starts_empty);
  RUN_TEST(test_miss_returns_the_last_value);
  RUN_TEST(test_hit_resets_the_miss_run);
  RUN_TEST(test_partial_write_is_not_a_hit);
  RUN_TEST(test_miss_counter_saturates);
  return UNITY_END();
}
