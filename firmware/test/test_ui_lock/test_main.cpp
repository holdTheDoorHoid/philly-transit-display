// Host tests for the display task's lock policy and its last-good fallback (DESIGN.md SS5, SS12.1).
//
// `pio test -e native -f test_ui_lock`. The pure half of src/app/ui_lock.h is deliberately free of
// Arduino and FreeRTOS - NATIVE_TEST compiles the semaphore half out - so the two things worth
// pinning down can be exercised on the host: that the display task's budget really is zero (a
// "short" wait still times out, and the assert this whole pass exists to remove is reachable only
// from a take that times out), and that a miss hands back the previous value rather than a
// default-constructed one.
#include <unity.h>

#include <memory>
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

// THE CONFIG HANDOVER'S CORRECTNESS PROPERTY, in the one form the host can hold (0.3.2-rc3).
//
// ui::tick() no longer copies a Config across the handover: config_store publishes one
// shared_ptr<const Config> and the display task borrows it through activeConfigPtr(), which is a
// LastGood<shared_ptr> read exactly like snapshotPtr(). That makes a MISS indistinguishable from
// "nothing changed" if you look at the return value alone - both hand back the pointer the task
// already holds. So tick() must decide from the POINTER, not from the flag: `g_pending` is
// cleared only when the pointer it gets back differs from the one it is rendering, which is what
// makes a miss cost one tick of delay instead of losing the save entirely.
//
// The device cannot demonstrate a lock miss on demand; this can, and it is the same arithmetic.
void test_a_missed_config_read_hands_back_the_pointer_you_already_have(void) {
  LastGood<std::shared_ptr<int>> last;
  auto boot = std::make_shared<int>(1);
  auto saved = std::make_shared<int>(2);

  // A successful read on the display task: the slot is assigned under the lock, then hit().
  last.slot() = boot;
  last.hit();
  std::shared_ptr<int> held = last.value();
  TEST_ASSERT_TRUE(held == boot);

  // A MISS while a save is pending. The value handed back is the one already held, so a tick that
  // cleared its pending flag here would drop the save for good.
  last.miss();
  TEST_ASSERT_TRUE(last.value() == held);
  TEST_ASSERT_EQUAL_UINT32(1, last.misses());
  TEST_ASSERT_TRUE(last.value() != saved);  // the pointer comparison tick() actually makes

  // The next tick gets the lock and the new pointer arrives, which is when the flag may clear.
  last.slot() = saved;
  last.hit();
  TEST_ASSERT_TRUE(last.value() == saved);
  TEST_ASSERT_TRUE(last.value() != held);
  TEST_ASSERT_EQUAL_UINT32(0, last.misses());
}

// And the reason the outgoing pointer is moved out of the slot before the new one is stored:
// assigning over it would drop the last reference INSIDE the critical section, running a whole
// Config's destructor there. Moved out first, the store is a refcount bump and the free happens
// after the give. Use counts are what say so.
void test_the_outgoing_config_is_released_outside_the_lock(void) {
  LastGood<std::shared_ptr<int>> last;
  auto published = std::make_shared<int>(7);
  {
    auto outgoing = std::make_shared<int>(6);
    last.slot() = outgoing;
    last.hit();
    TEST_ASSERT_EQUAL_INT(2, (int)outgoing.use_count());  // the slot and this local

    // What activeConfigPtr() does on the display task: move the slot's reference OUT, then store.
    std::shared_ptr<int> carried = std::move(last.slot());
    last.slot() = published;  // a refcount bump, nothing freed
    last.hit();
    TEST_ASSERT_EQUAL_INT(2, (int)published.use_count());
    TEST_ASSERT_TRUE(carried == outgoing);
    // `carried` dies at the end of this scope - i.e. after the give, in the real code.
  }
  TEST_ASSERT_TRUE(last.value() == published);
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
  RUN_TEST(test_a_missed_config_read_hands_back_the_pointer_you_already_have);
  RUN_TEST(test_the_outgoing_config_is_released_outside_the_lock);
  return UNITY_END();
}
