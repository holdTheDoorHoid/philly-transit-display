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
  // Below the free-heap floor, whatever the block looks like.
  TEST_ASSERT_FALSE(shouldRearmReserve(false, 12 * 1024, 24 * 1024));
}

void test_the_wedged_board_can_rearm_its_reserve() {
  // THE 0.3.2-rc1 CORRECTION. On the owner's board on 2026-09-17 `heap_reserve_held` read false
  // for the whole wedge: free8 17-20 KB with a 3,444 B largest block is under BOTH of the old
  // thresholds (20,480 and 4,340), so the reserve could not come back in the one state it exists
  // for. The reserve only has to be CARVED from the block, and it is 1,024 B.
  TEST_ASSERT_TRUE(shouldRearmReserve(false, 18320, 3444));   // the measured wedge state
  TEST_ASSERT_TRUE(shouldRearmReserve(false, 17076, 3444));
  // ...but not once the heap really has nothing left to give.
  TEST_ASSERT_FALSE(shouldRearmReserve(false, 18320, 2000));
  TEST_ASSERT_FALSE(shouldRearmReserve(false, 9000, 3444));
}

void test_the_rearm_floor_clears_the_gate_it_exists_to_get_past() {
  // THE INVARIANT. web_server.cpp refuses heavy reads below kMinHeavyResponseFree8 (12 KB) and
  // kMinHeavyResponseBlock (7,924 B). Re-arming must never be the allocation that pushes a device
  // back under that floor, so the re-arm threshold has to clear it by more than the reserve is
  // big. If someone lowers these constants, this fails rather than the device does.
  TEST_ASSERT_TRUE(kHeapReserveRearmFree8 > 12u * 1024u + kHeapReserveBytes);
  TEST_ASSERT_TRUE(kHeapReserveRearmBlock > kHeapReserveBytes);
  // Both thresholds moved DOWN in 0.3.2-rc1 so the reserve can come back on a fragmented heap.
  // The free-heap one is still the smallest value on the lattice that clears the invariant above,
  // which is why it is 13,556 and not the 12 KB it would be if the gate were the only constraint.
  TEST_ASSERT_EQUAL_UINT32(244u, (uint32_t)(kHeapReserveRearmFree8 % 512u));
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
  RUN_TEST(test_the_wedged_board_can_rearm_its_reserve);
  RUN_TEST(test_the_rearm_floor_clears_the_gate_it_exists_to_get_past);
  return UNITY_END();
}
