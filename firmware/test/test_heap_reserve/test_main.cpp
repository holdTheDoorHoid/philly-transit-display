// Host tests for the error-reply reserve's arm/disarm policy (DESIGN.md SS12.1, the third
// uncatchable-OOM instance: "the 503 for out of memory needs memory").
//
// `pio test -e native -f test_heap_reserve`. Like poller_liveness.h and proxy_queue.h, the header
// under test is deliberately pure - no Arduino, no FreeRTOS, no globals - so the DECISION can be
// included straight from src/ and exercised here. The kilobyte itself lives in heap_reserve.cpp
// and needs the device.
#include <unity.h>

#include "../../src/app/heap_reserve.h"

using transit_app::kHeapReserveBytes;
using transit_app::kHeapReserveRearmBlock;
using transit_app::kHeapReserveRearmFree8;
using transit_app::shouldRearmReserve;

void setUp(void) {}
void tearDown(void) {}

void test_a_held_reserve_is_never_rearmed() {
  // Idempotence matters because the poller asks four times a second. Re-arming while it is already
  // held would leak a kilobyte per call.
  TEST_ASSERT_FALSE(shouldRearmReserve(true, 200 * 1024, 100 * 1024));
  TEST_ASSERT_FALSE(shouldRearmReserve(true, 0, 0));
}

void test_a_spent_reserve_comes_back_on_a_healthy_heap() {
  TEST_ASSERT_TRUE(shouldRearmReserve(false, kHeapReserveRearmFree8, kHeapReserveRearmBlock));
  TEST_ASSERT_TRUE(shouldRearmReserve(false, 40 * 1024, 28 * 1024));  // a resting board
}

void test_it_waits_while_either_reading_is_low() {
  // Both halves, independently: a big block in a nearly-full heap is not a healthy heap, and
  // plenty of free bytes in tiny pieces is not one either.
  TEST_ASSERT_FALSE(shouldRearmReserve(false, kHeapReserveRearmFree8 - 1, kHeapReserveRearmBlock));
  TEST_ASSERT_FALSE(shouldRearmReserve(false, kHeapReserveRearmFree8, kHeapReserveRearmBlock - 1));
  // The wedge the audit measured: 13.7 KB free, 3,444 B largest. Exactly when a 503 is most likely
  // to be needed - and exactly when taking a kilobyte back would be the wrong thing to do.
  TEST_ASSERT_FALSE(shouldRearmReserve(false, 13 * 1024 + 716, 3444));
}

void test_the_rearm_floor_clears_the_gate_it_exists_to_get_past() {
  // THE INVARIANT. web_server.cpp refuses heavy reads below kMinHeavyResponseFree8 (12 KB) and
  // kMinHeavyResponseBlock (7,924 B). Re-arming must never be the allocation that pushes a device
  // back under that floor, so the re-arm threshold has to clear it by more than the reserve is
  // big. If someone lowers these constants, this fails rather than the device does.
  TEST_ASSERT_TRUE(kHeapReserveRearmFree8 > 12u * 1024u + kHeapReserveBytes);
  TEST_ASSERT_TRUE(kHeapReserveRearmBlock > kHeapReserveBytes);
  // And the block threshold sits mid-gap on the 512-byte lattice largest-block readings land on
  // (DESIGN.md SS2.1: real values are 500 + 512k, so mid-gap is 756 + 512k == 244 mod 512), rather
  // than on a round number that can refuse a real resting value by a handful of bytes - which is
  // the mistake kMinHeavyResponseBlock's 7,924 and kIdleWorkMinLargestBlock's 12,020 avoid.
  TEST_ASSERT_EQUAL_UINT32(244u, (uint32_t)(kHeapReserveRearmBlock % 512u));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_held_reserve_is_never_rearmed);
  RUN_TEST(test_a_spent_reserve_comes_back_on_a_healthy_heap);
  RUN_TEST(test_it_waits_while_either_reading_is_low);
  RUN_TEST(test_the_rearm_floor_clears_the_gate_it_exists_to_get_past);
  return UNITY_END();
}
