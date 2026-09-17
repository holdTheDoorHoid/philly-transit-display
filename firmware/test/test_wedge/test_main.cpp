// Host tests for the heap-wedge self-heal rule (wedge_policy.h, DESIGN.md SS12.1).
// `pio test -e native -f test_wedge`.
//
// Like poller_liveness.h, proxy_queue.h, heap_reserve.h and admission.h, the DECISION is pure and
// lives in a header with no Arduino in it, so it can be included straight from src/ and exercised
// here. The counters it feeds and the ESP.restart() it leads to stay in net_poller.cpp.
//
// These exist because the old inline rule was wrong on hardware in a way nobody noticed for two
// days: the board sat wedged from 15:50 to 16:07 on 2026-09-17, every poll failing at oom-transit
// with a 3,444-4,596 B largest block, and never restarted itself. wedge_policy.h's header carries
// the trace; the cases below pin each step of it.
#include <unity.h>

#include "../../src/app/wedge_policy.h"

using transit_app::kOomPollsBeforeReboot;
using transit_app::kStarvedPollsBeforeReboot;
using transit_app::kWedgeLargestBlock;
using transit_app::nextWedgeState;
using transit_app::WedgeReason;
using transit_app::WedgeState;
using transit_app::wedgeVerdict;

void setUp(void) {}
void tearDown(void) {}

// A healthy cycle: no OOM, poll fine, heap fine.
static WedgeState good(WedgeState s) { return nextWedgeState(s, false, false, true, 24 * 1024); }
// The 2026-09-17 cycle, exactly as the ring recorded it: caught bad_alloc at oom-transit, poll
// reported failure, largest free block 3,444 B.
static WedgeState oomCycle(WedgeState s) { return nextWedgeState(s, false, true, false, 3444); }

void test_three_out_of_memory_cycles_restart_the_board() {
  // THE HEADLINE. Counted from the fact the cycle knows about itself, so nothing about the heap
  // reading afterwards can stop the tally advancing.
  WedgeState s;
  s = oomCycle(s);
  TEST_ASSERT_EQUAL_UINT32(1, s.oom_streak);
  TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::None);
  s = oomCycle(s);
  TEST_ASSERT_EQUAL_UINT32(2, s.oom_streak);
  TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::None);
  s = oomCycle(s);
  TEST_ASSERT_EQUAL_UINT32(3, s.oom_streak);
  TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::OutOfMemory);
  TEST_ASSERT_EQUAL_UINT32(3, kOomPollsBeforeReboot);
}

void test_an_out_of_memory_cycle_counts_however_the_heap_reads_afterwards() {
  // The correction. The old rule re-derived "this cycle ran out of memory" from the largest free
  // block measured AFTER the cycle, which is a second condition that can lapse on its own - the
  // heap recovers a 7 KB block between the throw and the check, the tally zeroes, and fourteen
  // cycles of evidence go with it. A caught bad_alloc now counts at any heap reading at all.
  WedgeState s;
  s = nextWedgeState(s, false, true, false, 3444);       // starved as well
  s = nextWedgeState(s, false, true, false, 40 * 1024);  // roomy afterwards - still counts
  s = nextWedgeState(s, false, true, true, 64 * 1024);   // and even if the poll reported OK
  TEST_ASSERT_EQUAL_UINT32(3, s.oom_streak);
  TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::OutOfMemory);
}

void test_one_good_cycle_clears_the_out_of_memory_tally() {
  // Consecutive, not cumulative: a board that got memory again is not wedged, and the point of the
  // restart is to recover a heap that cannot be recovered any other way.
  WedgeState s;
  s = oomCycle(s);
  s = oomCycle(s);
  s = good(s);
  TEST_ASSERT_EQUAL_UINT32(0, s.oom_streak);
  s = oomCycle(s);
  s = oomCycle(s);
  TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::None);
}

void test_the_non_throwing_wedge_keeps_its_slow_threshold() {
  // A heap so cut up that fetches fail without any single allocation being large enough to raise
  // bad_alloc. This rule's condition can also be met by an ordinary SEPTA outage on a board whose
  // heap merely happens to be busy, so it stays hard to trip: fifteen, not three.
  WedgeState s;
  for (uint32_t i = 1; i < kStarvedPollsBeforeReboot; ++i) {
    s = nextWedgeState(s, false, false, false, kWedgeLargestBlock - 1);
    TEST_ASSERT_EQUAL_UINT32(i, s.starved_streak);
    TEST_ASSERT_EQUAL_UINT32(0, s.oom_streak);
    TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::None);
  }
  s = nextWedgeState(s, false, false, false, kWedgeLargestBlock - 1);
  TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::Starved);
}

void test_a_plain_outage_never_reboots() {
  // Every poll failing on a perfectly healthy heap is a SEPTA outage, and rebooting for it would
  // replace a stale screen with a stale screen plus a reboot loop.
  WedgeState s;
  for (int i = 0; i < 100; ++i) {
    s = nextWedgeState(s, false, false, false, 24 * 1024);
    TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::None);
  }
  TEST_ASSERT_EQUAL_UINT32(0, s.starved_streak);
  TEST_ASSERT_EQUAL_UINT32(0, s.oom_streak);
}

void test_an_upload_stands_down_and_forgets() {
  // An OTA takes the heap for a ~1.7 MB write, which is exactly the condition these look for.
  // Restarting mid-Update.write() is not a brick (the boot partition only switches at
  // Update.end(true)) but it throws the owner's upload away and looks like a crash. Both tallies
  // are zeroed and FORGOTTEN, not suspended: what the heap was doing before an upload is not
  // evidence about what it is doing after.
  WedgeState s;
  s = oomCycle(s);
  s = oomCycle(s);
  TEST_ASSERT_EQUAL_UINT32(2, s.oom_streak);
  s = nextWedgeState(s, true, true, false, 3444);  // ota_busy
  TEST_ASSERT_EQUAL_UINT32(0, s.oom_streak);
  TEST_ASSERT_EQUAL_UINT32(0, s.starved_streak);
  TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::None);
}

void test_the_out_of_memory_rule_wins_when_both_fire() {
  // Both conditions are usually true together - an OOM cycle reports failure and leaves a small
  // block - and the restart note should say the more specific of the two.
  WedgeState s{kOomPollsBeforeReboot, kStarvedPollsBeforeReboot};
  TEST_ASSERT_TRUE(wedgeVerdict(s) == WedgeReason::OutOfMemory);
}

void test_the_thresholds_are_the_measured_trade() {
  // Why three and not fifteen, as arithmetic rather than as a comment. A failed poll drives
  // nextIntervalS()'s backoff, which saturates at kBaseBackoffS << 3 = 240 s after four
  // consecutive failures. So the time a threshold of N really costs is:
  //     30 + 60 + 120 + (N - 3) * 240  seconds
  // Fifteen is 51 minutes, which is what the owner's board was doing on 2026-09-17 while everyone
  // expected 7.5. Three is 210 s. If the backoff constants ever change, this is the test that
  // should be revisited with them.
  auto secondsFor = [](uint32_t n) -> uint32_t {
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t step = i == 0 ? 30u : (i == 1 ? 60u : (i == 2 ? 120u : 240u));
      total += step;
    }
    return total;
  };
  TEST_ASSERT_EQUAL_UINT32(210, secondsFor(kOomPollsBeforeReboot));
  TEST_ASSERT_EQUAL_UINT32(3090, secondsFor(kStarvedPollsBeforeReboot));  // 51.5 minutes
  TEST_ASSERT_TRUE(secondsFor(kOomPollsBeforeReboot) < 5 * 60);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_three_out_of_memory_cycles_restart_the_board);
  RUN_TEST(test_an_out_of_memory_cycle_counts_however_the_heap_reads_afterwards);
  RUN_TEST(test_one_good_cycle_clears_the_out_of_memory_tally);
  RUN_TEST(test_the_non_throwing_wedge_keeps_its_slow_threshold);
  RUN_TEST(test_a_plain_outage_never_reboots);
  RUN_TEST(test_an_upload_stands_down_and_forgets);
  RUN_TEST(test_the_out_of_memory_rule_wins_when_both_fire);
  RUN_TEST(test_the_thresholds_are_the_measured_trade);
  return UNITY_END();
}
