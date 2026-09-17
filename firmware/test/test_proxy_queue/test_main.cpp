// Host tests for the queued-job policy (DESIGN.md SS12.1 "Stats/proxy job queue").
//
// `pio test -e native -f test_proxy_queue`. Like poller_liveness.h, the header under test is
// deliberately pure - no Arduino, no FreeRTOS, no globals - so it can be included straight from
// src/ and exercised here; that is the whole reason the decision was lifted out of the `&&` it
// used to live in.
#include <unity.h>

#include "../../src/app/proxy_queue.h"

using transit_app::decideQueuedJob;
using transit_app::kMaxJobDeferrals;
using transit_app::QueuedJobAction;

void setUp(void) {}
void tearDown(void) {}

void test_a_live_client_with_headroom_runs() {
  TEST_ASSERT_TRUE(decideQueuedJob(true, true, 0) == QueuedJobAction::Run);
  TEST_ASSERT_TRUE(decideQueuedJob(true, true, kMaxJobDeferrals) == QueuedJobAction::Run);
}

void test_a_dead_client_is_dropped_whatever_the_heap_is_doing() {
  TEST_ASSERT_TRUE(decideQueuedJob(false, true, 0) == QueuedJobAction::DropClientGone);
  TEST_ASSERT_TRUE(decideQueuedJob(false, false, 0) == QueuedJobAction::DropClientGone);
  // Even after waiting out the whole budget: there is nobody to answer.
  TEST_ASSERT_TRUE(decideQueuedJob(false, false, kMaxJobDeferrals) == QueuedJobAction::DropClientGone);
}

void test_no_headroom_waits_a_bounded_number_of_slices_then_answers() {
  // THE REGRESSION GUARD, in its 0.3.1-rc3 form. A job that cannot start now WAITS - the burst
  // that is blocking it is over in seconds, and the Stats page would rather be slow than wrong -
  // but the waiting is bounded, because a queued job holds a paused AsyncWebServerRequest whose
  // server-side timeout the library turned off when it paused it. Nothing else can ever end it, so
  // "wait" must always become an answer (audit_runtime SS4 is the hang this replaced).
  TEST_ASSERT_TRUE(decideQueuedJob(true, false, 0) == QueuedJobAction::Defer);
  TEST_ASSERT_TRUE(decideQueuedJob(true, false, kMaxJobDeferrals - 1) == QueuedJobAction::Defer);
  TEST_ASSERT_TRUE(decideQueuedJob(true, false, kMaxJobDeferrals) == QueuedJobAction::Refuse503);
  TEST_ASSERT_TRUE(decideQueuedJob(true, false, 255) == QueuedJobAction::Refuse503);
}

void test_the_deferral_budget_is_shorter_than_a_client_timeout() {
  // The idle slice is a 250 ms wait, and the device suite gives a request about 8 s. Answering
  // before the client gives up is the difference between a 503 it can act on and a dangling
  // socket, so the budget has to stay under that.
  TEST_ASSERT_TRUE(kMaxJobDeferrals > 0);
  TEST_ASSERT_TRUE((unsigned)kMaxJobDeferrals * 250u < 8000u);
}

void test_every_outcome_disposes_of_the_job() {
  // Stated as an exhaustive check over the two inputs: whatever the caller passes, the job is
  // dealt with. If an outcome is ever added, this fails until it is thought about.
  for (int alive = 0; alive < 2; ++alive) {
    for (int heap = 0; heap < 2; ++heap) {
      for (int d = 0; d < 2; ++d) {
        QueuedJobAction a = decideQueuedJob(alive != 0, heap != 0,
                                             d == 0 ? 0 : (uint8_t)kMaxJobDeferrals);
        bool known = a == QueuedJobAction::Run || a == QueuedJobAction::DropClientGone ||
                     a == QueuedJobAction::Defer || a == QueuedJobAction::Refuse503;
        TEST_ASSERT_TRUE(known);
      }
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_live_client_with_headroom_runs);
  RUN_TEST(test_a_dead_client_is_dropped_whatever_the_heap_is_doing);
  RUN_TEST(test_no_headroom_waits_a_bounded_number_of_slices_then_answers);
  RUN_TEST(test_the_deferral_budget_is_shorter_than_a_client_timeout);
  RUN_TEST(test_every_outcome_disposes_of_the_job);
  return UNITY_END();
}
