// Host tests for accept-time admission control (DESIGN.md SS12.1, the fourth uncatchable-OOM
// instance). `pio test -e native -f test_admission`.
//
// Like poller_liveness.h, proxy_queue.h and heap_reserve.h, the DECISION is pure and lives in a
// header with no Arduino in it, so it can be included straight from src/ and exercised here. The
// weak_ptr sweep that measures `in_flight` needs the library and stays in web_server.cpp.
#include <unity.h>

#include "../../src/app/admission.h"

using transit_app::admitConnection;
using transit_app::kAcceptMinFree8;
using transit_app::kAcceptMinLargestBlock;
using transit_app::kMaxInFlightRequests;
using transit_app::kRequestCostBytes;
using transit_app::kSendBufferBytes;

void setUp(void) {}
void tearDown(void) {}

void test_a_healthy_board_admits() {
  TEST_ASSERT_TRUE(admitConnection(0, 40 * 1024, 24 * 1024));
  TEST_ASSERT_TRUE(admitConnection(kMaxInFlightRequests - 1, 40 * 1024, 24 * 1024));
}

void test_the_floor_still_covers_a_concurrent_request() {
  // The floors did not move, only when they apply. Once a request is in flight, a second one is
  // admitted only if the heap can still answer it - which is what rc2's crash was about.
  TEST_ASSERT_TRUE(admitConnection(1, kAcceptMinFree8, kAcceptMinLargestBlock));
  TEST_ASSERT_FALSE(admitConnection(1, kAcceptMinFree8 - 1, kAcceptMinLargestBlock));
  TEST_ASSERT_FALSE(admitConnection(1, kAcceptMinFree8, kAcceptMinLargestBlock - 1));
}

void test_the_cap_is_hard() {
  // However much heap there is. The cap bounds what the admitted requests are ABOUT to allocate -
  // their documents, response objects and 2,872 B send buffers - which is exactly what a free-heap
  // reading at accept time cannot see, because at accept a request has only cost ~760 B.
  TEST_ASSERT_FALSE(admitConnection(kMaxInFlightRequests, 200 * 1024, 100 * 1024));
  TEST_ASSERT_FALSE(admitConnection(kMaxInFlightRequests + 1, 200 * 1024, 100 * 1024));
}

void test_either_heap_floor_refuses_on_its_own() {
  // ...while a request is already in flight. With none, the floors do not apply at all - see
  // test_the_only_connection_is_always_admitted below.
  TEST_ASSERT_FALSE(admitConnection(1, kAcceptMinFree8 - 1, 24 * 1024));
  TEST_ASSERT_FALSE(admitConnection(1, 40 * 1024, kAcceptMinLargestBlock - 1));
  // Exactly at both floors is admitted: they are minimums, not exclusive bounds.
  TEST_ASSERT_TRUE(admitConnection(1, kAcceptMinFree8, kAcceptMinLargestBlock));
}

void test_the_only_connection_is_always_admitted() {
  // THE 0.3.2-rc1 RULE, and the failure it exists for. On the owner's board at v0.3.1 the largest
  // free block decayed to 3,444 B, which is under kAcceptMinLargestBlock, and the accept path then
  // refused EVERY connection - /api/debug/ui (the endpoint kept outside refuseIfLowHeap so it
  // still answers on a starved heap) and POST /api/reboot (the recovery path) included. The board
  // answered ping and nothing else for five minutes.
  //
  // One request at a time is the guaranteed service level: it cannot reproduce the concurrent-burst
  // crash the floors exist for, and if its own reply will not fit, guarded() answers 503 out of the
  // 1 KB reserve.
  TEST_ASSERT_TRUE(admitConnection(0, 18320, 3444));  // the measured v0.3.1 lockout state
  TEST_ASSERT_TRUE(admitConnection(0, 2000, 1500));
  TEST_ASSERT_TRUE(admitConnection(0, 0, 0));
}

void test_the_state_that_crashed_rc2_is_refused() {
  // Serial from the rc2 suite run, moments before the abort: a queued job had just been refused at
  // free8 19,528 / largest 11,252, and the burst then drove the heap to nothing with seven requests
  // alive. Seven is past the cap whatever the heap says - which is the defence.
  TEST_ASSERT_FALSE(admitConnection(7, 19528, 11252));
  TEST_ASSERT_FALSE(admitConnection(6, 19528, 11252));
  // And with the heap gone, a SECOND concurrent request is still refused - the lockout fix relaxes
  // the floors for the first connection only.
  TEST_ASSERT_FALSE(admitConnection(1, 2000, 1500));
  TEST_ASSERT_FALSE(admitConnection(1, 18320, 3444));
}

void test_the_rule_table() {
  // The whole decision as four rows, so a future edit that changes one of them fails here rather
  // than on the hardware. Columns: in_flight, free8, largest -> admitted.
  struct Row { uint32_t in_flight; size_t free8; size_t largest; bool admit; };
  static const Row kRows[] = {
    {0, 40 * 1024, 24 * 1024, true},   // idle, healthy
    {0, 1024, 512, true},              // idle, starved: still admitted (the guaranteed service level)
    {1, 40 * 1024, 24 * 1024, true},   // busy, healthy
    {1, 40 * 1024, 1024, false},       // busy, fragmented: the burst defence
    {1, 1024, 24 * 1024, false},       // busy, nearly full
    {kMaxInFlightRequests - 1, 40 * 1024, 24 * 1024, true},   // the last slot
    {kMaxInFlightRequests, 200 * 1024, 100 * 1024, false},    // the cap, whatever the heap says
  };
  for (const Row &r : kRows) {
    TEST_ASSERT_EQUAL_MESSAGE(r.admit, admitConnection(r.in_flight, r.free8, r.largest),
                               "admission rule table row changed");
  }
}

void test_the_floor_covers_what_one_request_actually_costs() {
  // THE INVARIANT. The floor exists so that a connection we admit can still be ANSWERED - the
  // answer being where rc2 died, in the library's deferred header assembly. So it has to cover one
  // request's own cost plus the send buffer it will ask for later. If someone lowers either
  // constant, this fails rather than the board does.
  TEST_ASSERT_TRUE(kAcceptMinFree8 >= kRequestCostBytes + kSendBufferBytes);
  TEST_ASSERT_TRUE(kAcceptMinLargestBlock > kSendBufferBytes);
  // Neither threshold sits on a round 512-byte boundary: heap readings land on a lattice and a
  // round number refuses a real resting value by a handful of bytes (DESIGN.md SS2.1).
  TEST_ASSERT_NOT_EQUAL(0, (int)(kAcceptMinFree8 % 512));
  TEST_ASSERT_NOT_EQUAL(0, (int)(kAcceptMinLargestBlock % 512));
}

void test_the_cap_respects_the_suites_refusal_budget() {
  // Section E fires 7 concurrent requests per round for 3 rounds and allows at most 6 refusals in
  // total. A cap of N refuses about (7 - N) per round once the burst is simultaneous, so the cap
  // cannot go below 5 without failing that check by construction. Written as a test because the
  // number looks arbitrary otherwise, and because lowering it is the obvious "safer" edit.
  const unsigned per_round = 7u > kMaxInFlightRequests ? 7u - kMaxInFlightRequests : 0u;
  TEST_ASSERT_TRUE(3u * per_round <= 6u);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_board_admits);
  RUN_TEST(test_the_floor_still_covers_a_concurrent_request);
  RUN_TEST(test_the_cap_is_hard);
  RUN_TEST(test_either_heap_floor_refuses_on_its_own);
  RUN_TEST(test_the_only_connection_is_always_admitted);
  RUN_TEST(test_the_state_that_crashed_rc2_is_refused);
  RUN_TEST(test_the_rule_table);
  RUN_TEST(test_the_floor_covers_what_one_request_actually_costs);
  RUN_TEST(test_the_cap_respects_the_suites_refusal_budget);
  return UNITY_END();
}
