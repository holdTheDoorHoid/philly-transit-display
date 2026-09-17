// Host tests for the queued-job policy (DESIGN.md SS12.1 "Stats/proxy job queue").
//
// `pio test -e native -f test_proxy_queue`. Like poller_liveness.h, the header under test is
// deliberately pure - no Arduino, no FreeRTOS, no globals - so it can be included straight from
// src/ and exercised here; that is the whole reason the decision was lifted out of the `&&` it
// used to live in.
#include <unity.h>

#include "../../src/app/proxy_queue.h"

using transit_app::decideQueuedJob;
using transit_app::QueuedJobAction;

void setUp(void) {}
void tearDown(void) {}

void test_a_live_client_with_headroom_runs() {
  TEST_ASSERT_TRUE(decideQueuedJob(true, true) == QueuedJobAction::Run);
}

void test_a_dead_client_is_dropped_whatever_the_heap_is_doing() {
  TEST_ASSERT_TRUE(decideQueuedJob(false, true) == QueuedJobAction::DropClientGone);
  TEST_ASSERT_TRUE(decideQueuedJob(false, false) == QueuedJobAction::DropClientGone);
}

void test_no_headroom_answers_rather_than_waiting() {
  // THE REGRESSION GUARD. There is no "leave it on the queue" outcome, and there must never be
  // one: a queued job holds a paused AsyncWebServerRequest whose server-side timeout the library
  // turned off when it paused it, so nothing else can ever end it, and with both of the two slots
  // held every later stats/proxy request is answered "busy" for the rest of the device's uptime
  // (audit_runtime SS4). Refusing promptly gives the slot back and releases the request.
  TEST_ASSERT_TRUE(decideQueuedJob(true, false) == QueuedJobAction::Refuse503);
}

void test_every_outcome_disposes_of_the_job() {
  // Stated as an exhaustive check over the two inputs: whatever the caller passes, the job is
  // dealt with. If an outcome is ever added, this fails until it is thought about.
  for (int alive = 0; alive < 2; ++alive) {
    for (int heap = 0; heap < 2; ++heap) {
      QueuedJobAction a = decideQueuedJob(alive != 0, heap != 0);
      bool known = a == QueuedJobAction::Run || a == QueuedJobAction::DropClientGone ||
                   a == QueuedJobAction::Refuse503;
      TEST_ASSERT_TRUE(known);
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_live_client_with_headroom_runs);
  RUN_TEST(test_a_dead_client_is_dropped_whatever_the_heap_is_doing);
  RUN_TEST(test_no_headroom_answers_rather_than_waiting);
  RUN_TEST(test_every_outcome_disposes_of_the_job);
  return UNITY_END();
}
