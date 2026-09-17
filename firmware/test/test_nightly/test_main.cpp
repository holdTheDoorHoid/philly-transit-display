// Host tests for the nightly restart rule (nightly_restart.h, DESIGN.md SS12.1).
// `pio test -e native -f test_nightly`.
//
// The decision is pure - the clock read and the restart itself are main.cpp's - so every guard can
// be exercised here rather than by waiting until 03:30 on the hardware. That matters more than
// usual for this one: a rule that restarts the device is a rule where a mistake looks like a boot
// loop, and a boot loop on a device whose recovery path is HTTP is not a cheap mistake.
#include <unity.h>

#include "../../src/app/nightly_restart.h"
#include "daypart_core/daypart.h"

using transit_app::kNightlyMinUptimeS;
using transit_app::shouldRestartNightly;

void setUp(void) {}
void tearDown(void) {}

constexpr int kHalfThree = 3 * 60 + 30;  // 03:30, the default
constexpr uint32_t kUp = kNightlyMinUptimeS + 1;

void test_it_fires_at_the_configured_minute() {
  TEST_ASSERT_TRUE(shouldRestartNightly(true, kHalfThree, kHalfThree, -1, kUp, false));
  // ...and at no other minute of the day.
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, kHalfThree - 1, -1, kUp, false));
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, kHalfThree + 1, -1, kUp, false));
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, 0, -1, kUp, false));
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, 23 * 60 + 59, -1, kUp, false));
}

void test_the_time_comes_from_the_same_parser_the_rest_of_the_config_uses() {
  // device.nightly_restart.time is an "HH:MM" string like quiet.start and the profile windows, and
  // it goes through daypart::parseClock() exactly as they do - so midnight, a leading zero and a
  // malformed value all behave the way they already do everywhere else.
  TEST_ASSERT_EQUAL_INT(kHalfThree, daypart::parseClock("03:30"));
  TEST_ASSERT_EQUAL_INT(0, daypart::parseClock("00:00"));
  TEST_ASSERT_EQUAL_INT(-1, daypart::parseClock("3:30"));
  TEST_ASSERT_EQUAL_INT(-1, daypart::parseClock("24:00"));
  TEST_ASSERT_EQUAL_INT(-1, daypart::parseClock(""));
  // Midnight is a real minute, not a falsy one: 0 must fire at 0.
  TEST_ASSERT_TRUE(shouldRestartNightly(true, 0, 0, -1, kUp, false));
}

void test_an_unparseable_time_can_only_disable_it() {
  // parseClock() answers -1, and the rule treats -1 as "never". A bad value must not become
  // "restart at midnight" or "restart whenever" - the failure direction for a restart rule is
  // always "do not restart".
  TEST_ASSERT_FALSE(shouldRestartNightly(true, -1, kHalfThree, -1, kUp, false));
  TEST_ASSERT_FALSE(shouldRestartNightly(true, -1, 0, -1, kUp, false));
}

void test_it_considers_each_minute_once() {
  // loop() runs at ~1 Hz, so the matching minute is seen about sixty times. The rule declines every
  // pass after the first by comparing against the minute it last ran at, which the caller stores
  // whether or not it fired.
  TEST_ASSERT_TRUE(shouldRestartNightly(true, kHalfThree, kHalfThree, kHalfThree - 1, kUp, false));
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, kHalfThree, kHalfThree, kUp, false));
}

void test_a_board_that_just_booted_does_not_restart() {
  // THE BOOT-LOOP GUARD. Without it a board that comes up at 03:29 restarts at 03:30, comes up
  // again, and - if the clock steps around the boundary, or the restart is fast enough - does it
  // again. An hour is far more than boot + Wi-Fi + NTP + the first poll, and far less than the gap
  // between two nights.
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, kHalfThree, -1, 0, false));
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, kHalfThree, -1, 60, false));
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, kHalfThree, -1, kNightlyMinUptimeS, false));
  TEST_ASSERT_TRUE(shouldRestartNightly(true, kHalfThree, kHalfThree, -1, kNightlyMinUptimeS + 1, false));
  TEST_ASSERT_EQUAL_UINT32(3600, kNightlyMinUptimeS);
}

void test_no_clock_means_no_restart() {
  // Before NTP the local time is 1970; main.cpp passes -1 rather than a minute derived from it,
  // because "03:30" in 1970 is a moment with nothing to do with 03:30.
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, -1, -1, kUp, false));
}

void test_an_upload_stands_it_down() {
  // Same hazard as the wedge and liveness nets: restarting mid-Update.write() throws the owner's
  // upload away at the worst possible moment and looks exactly like a crash.
  TEST_ASSERT_FALSE(shouldRestartNightly(true, kHalfThree, kHalfThree, -1, kUp, true));
}

void test_disabled_means_disabled() {
  TEST_ASSERT_FALSE(shouldRestartNightly(false, kHalfThree, kHalfThree, -1, kUp, false));
  TEST_ASSERT_FALSE(shouldRestartNightly(false, 0, 0, -1, kUp * 100, false));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_it_fires_at_the_configured_minute);
  RUN_TEST(test_the_time_comes_from_the_same_parser_the_rest_of_the_config_uses);
  RUN_TEST(test_an_unparseable_time_can_only_disable_it);
  RUN_TEST(test_it_considers_each_minute_once);
  RUN_TEST(test_a_board_that_just_booted_does_not_restart);
  RUN_TEST(test_no_clock_means_no_restart);
  RUN_TEST(test_an_upload_stands_it_down);
  RUN_TEST(test_disabled_means_disabled);
  return UNITY_END();
}
