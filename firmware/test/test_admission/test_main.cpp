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
using transit_app::kAdmissionFloorsApplyFrom;
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
  // The floors did not move, only WHEN they apply - which since 0.3.2-rc3 is from the third
  // concurrent request. With two already in flight, a third is admitted only if the heap can
  // still answer it, which is what rc2's crash was about.
  TEST_ASSERT_TRUE(admitConnection(2, kAcceptMinFree8, kAcceptMinLargestBlock));
  TEST_ASSERT_FALSE(admitConnection(2, kAcceptMinFree8 - 1, kAcceptMinLargestBlock));
  TEST_ASSERT_FALSE(admitConnection(2, kAcceptMinFree8, kAcceptMinLargestBlock - 1));
}

void test_the_cap_is_hard() {
  // However much heap there is. The cap bounds what the admitted requests are ABOUT to allocate -
  // their documents, response objects and 2,872 B send buffers - which is exactly what a free-heap
  // reading at accept time cannot see, because at accept a request has only cost ~760 B.
  TEST_ASSERT_FALSE(admitConnection(kMaxInFlightRequests, 200 * 1024, 100 * 1024));
  TEST_ASSERT_FALSE(admitConnection(kMaxInFlightRequests + 1, 200 * 1024, 100 * 1024));
}

void test_either_heap_floor_refuses_on_its_own() {
  // ...once kAdmissionFloorsApplyFrom requests are already in flight. Below that they do not apply
  // at all - see test_the_first_two_connections_are_always_admitted below.
  TEST_ASSERT_FALSE(admitConnection(2, kAcceptMinFree8 - 1, 24 * 1024));
  TEST_ASSERT_FALSE(admitConnection(2, 40 * 1024, kAcceptMinLargestBlock - 1));
  // Exactly at both floors is admitted: they are minimums, not exclusive bounds.
  TEST_ASSERT_TRUE(admitConnection(2, kAcceptMinFree8, kAcceptMinLargestBlock));
}

void test_the_first_two_connections_are_always_admitted() {
  // THE 0.3.2-rc1 RULE, and the failure it exists for. On the owner's board at v0.3.1 the largest
  // free block decayed to 3,444 B, which is under kAcceptMinLargestBlock, and the accept path then
  // refused EVERY connection - /api/debug/ui (the endpoint kept outside refuseIfLowHeap so it
  // still answers on a starved heap) and POST /api/reboot (the recovery path) included. The board
  // answered ping and nothing else for five minutes.
  //
  // rc3 widens the guarantee from one request to two, because rc5's section E measured NINE
  // aborted connections against a budget of six and the three over budget were the floors refusing
  // a SECOND request on a heap that was down. Two alive at once is not the state that crashed rc2
  // (that took seven), and if the second one's own reply will not fit, guarded() answers 503 out
  // of the 1 KB reserve exactly as it does for the first.
  TEST_ASSERT_TRUE(admitConnection(0, 18320, 3444));  // the measured v0.3.1 lockout state
  TEST_ASSERT_TRUE(admitConnection(0, 2000, 1500));
  TEST_ASSERT_TRUE(admitConnection(0, 0, 0));
  TEST_ASSERT_TRUE(admitConnection(1, 18320, 3444));
  TEST_ASSERT_TRUE(admitConnection(1, 2000, 1500));
  TEST_ASSERT_TRUE(admitConnection(1, 0, 0));
}

void test_the_state_that_crashed_rc2_is_refused() {
  // Serial from the rc2 suite run, moments before the abort: a queued job had just been refused at
  // free8 19,528 / largest 11,252, and the burst then drove the heap to nothing with seven requests
  // alive. Seven is past the cap whatever the heap says - which is the defence.
  TEST_ASSERT_FALSE(admitConnection(7, 19528, 11252));
  TEST_ASSERT_FALSE(admitConnection(6, 19528, 11252));
  // And with the heap gone, a THIRD concurrent request is still refused - rc3 relaxes the floors
  // for the first two connections only, so the burst defence is intact from three onwards.
  TEST_ASSERT_FALSE(admitConnection(2, 2000, 1500));
  TEST_ASSERT_FALSE(admitConnection(2, 18320, 3444));
  TEST_ASSERT_FALSE(admitConnection(4, 18320, 3444));
}

void test_the_rule_table() {
  // The whole decision as four rows, so a future edit that changes one of them fails here rather
  // than on the hardware. Columns: in_flight, free8, largest -> admitted.
  struct Row { uint32_t in_flight; size_t free8; size_t largest; bool admit; };
  static const Row kRows[] = {
    {0, 40 * 1024, 24 * 1024, true},   // idle, healthy
    {0, 1024, 512, true},              // idle, starved: still admitted (the guaranteed service level)
    {1, 40 * 1024, 24 * 1024, true},   // one in flight, healthy
    {1, 40 * 1024, 1024, true},        // one in flight, fragmented: ADMITTED since 0.3.2-rc3
    {1, 1024, 24 * 1024, true},        // one in flight, nearly full: admitted too
    {2, 40 * 1024, 24 * 1024, true},   // contention, healthy
    {2, 40 * 1024, 1024, false},       // contention, fragmented: the burst defence
    {2, 1024, 24 * 1024, false},       // contention, nearly full
    {3, 40 * 1024, 1024, false},       // and it keeps applying, all the way to the cap
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
  TEST_ASSERT_EQUAL_UINT(2, per_round);
  TEST_ASSERT_TRUE(3u * per_round <= 6u);
}

void test_the_floors_do_not_add_refusals_on_top_of_the_cap() {
  // THE rc5 OVERRUN, AS ARITHMETIC. Section E fires 7 simultaneous requests per round, 3 rounds,
  // and allows at most 6 aborted connections in total. rc5 produced NINE - three per round. The
  // cap explains exactly two of those three (7 - 5); the extra one was the floors refusing the
  // SECOND request of the round, because the accept-time heap reading dipped under
  // kAcceptMinLargestBlock while the first request was still being answered. A request arriving
  // into one other request is not the contention the floors were derived for.
  //
  // Modelled here as the heap reading the accept path actually takes, per request: healthy except
  // at the moment the second connection arrives.
  auto refusalsInARound = [](bool floors_from_one) {
    unsigned refused = 0;
    for (uint32_t in_flight = 0; in_flight < 7; ++in_flight) {
      const size_t free8 = 40u * 1024u;
      // The dip: while one request is alive, the largest block is momentarily under the floor.
      const size_t largest = in_flight == 1 ? kAcceptMinLargestBlock - 1 : 24u * 1024u;
      bool admit;
      if (in_flight >= kMaxInFlightRequests) {
        admit = false;
      } else if (in_flight < (floors_from_one ? 1u : kAdmissionFloorsApplyFrom)) {
        admit = true;
      } else {
        admit = free8 >= kAcceptMinFree8 && largest >= kAcceptMinLargestBlock;
      }
      if (!admit) ++refused;
    }
    return refused;
  };

  // rc5's rule: three per round, nine across the suite's three rounds - the measured overrun.
  TEST_ASSERT_EQUAL_UINT(3, refusalsInARound(true));
  TEST_ASSERT_TRUE(3u * refusalsInARound(true) > 6u);

  // rc3's rule: the cap's two per round and nothing else, six in total, inside the budget.
  TEST_ASSERT_EQUAL_UINT(2, refusalsInARound(false));
  TEST_ASSERT_TRUE(3u * refusalsInARound(false) <= 6u);
}

// AND THE HONEST OTHER HALF, so nobody reads the test above as a promise the rule cannot keep.
// On a DEEPLY fragmented heap - section B's measured 19,528 free / 3,060 largest, under
// kAcceptMinLargestBlock by a wide margin and for minutes at a time rather than momentarily - the
// floors still refuse from the third request onward, which is five per round of a seven-deep
// burst. That is the burst defence doing its job, not a regression: the two events are different,
// and the fix for section B is that the heap no longer gets there (the config-save churn), not a
// looser floor.
void test_a_deeply_fragmented_heap_still_refuses_from_the_third() {
  TEST_ASSERT_TRUE(admitConnection(0, 19528, 3060));
  TEST_ASSERT_TRUE(admitConnection(1, 19528, 3060));
  TEST_ASSERT_FALSE(admitConnection(2, 19528, 3060));
  unsigned refused = 0;
  for (uint32_t in_flight = 0; in_flight < 7; ++in_flight) {
    if (!admitConnection(in_flight, 19528, 3060)) ++refused;
  }
  TEST_ASSERT_EQUAL_UINT(5, refused);
}

void test_the_floor_threshold_is_two() {
  // The number itself, so an edit that puts it back to 1 (reinstating the rc5 refusal overrun) or
  // pushes it to 3 (letting a third request past the floors the rc2 crash needed) fails here.
  TEST_ASSERT_EQUAL_UINT32(2, kAdmissionFloorsApplyFrom);
  TEST_ASSERT_TRUE(kAdmissionFloorsApplyFrom < kMaxInFlightRequests);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_board_admits);
  RUN_TEST(test_the_floor_still_covers_a_concurrent_request);
  RUN_TEST(test_the_cap_is_hard);
  RUN_TEST(test_either_heap_floor_refuses_on_its_own);
  RUN_TEST(test_the_first_two_connections_are_always_admitted);
  RUN_TEST(test_the_state_that_crashed_rc2_is_refused);
  RUN_TEST(test_the_rule_table);
  RUN_TEST(test_the_floor_covers_what_one_request_actually_costs);
  RUN_TEST(test_the_cap_respects_the_suites_refusal_budget);
  RUN_TEST(test_the_floors_do_not_add_refusals_on_top_of_the_cap);
  RUN_TEST(test_a_deeply_fragmented_heap_still_refuses_from_the_third);
  RUN_TEST(test_the_floor_threshold_is_two);
  return UNITY_END();
}
