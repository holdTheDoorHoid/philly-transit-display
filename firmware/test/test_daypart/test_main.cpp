#include <unity.h>

#include "daypart_core/daypart.h"

using namespace daypart;

void setUp(void) {}
void tearDown(void) {}

void test_parse_clock() {
  TEST_ASSERT_EQUAL_INT(0, parseClock("00:00"));
  TEST_ASSERT_EQUAL_INT(23 * 60 + 59, parseClock("23:59"));
  TEST_ASSERT_EQUAL_INT(5 * 60 + 30, parseClock("05:30"));
  TEST_ASSERT_EQUAL_INT(-1, parseClock("24:00"));
  TEST_ASSERT_EQUAL_INT(-1, parseClock("5:30"));
  TEST_ASSERT_EQUAL_INT(-1, parseClock("05-30"));
  TEST_ASSERT_EQUAL_INT(-1, parseClock(""));
}

void test_window_same_day_and_crossing_midnight() {
  int s = parseClock("09:00"), e = parseClock("17:00");
  TEST_ASSERT_TRUE(inWindow(parseClock("09:00"), s, e));
  TEST_ASSERT_TRUE(inWindow(parseClock("16:59"), s, e));
  TEST_ASSERT_FALSE(inWindow(parseClock("17:00"), s, e));
  TEST_ASSERT_FALSE(inWindow(parseClock("03:00"), s, e));
  int qs = parseClock("23:00"), qe = parseClock("06:00");  // quiet hours
  TEST_ASSERT_TRUE(inWindow(parseClock("23:30"), qs, qe));
  TEST_ASSERT_TRUE(inWindow(parseClock("00:00"), qs, qe));
  TEST_ASSERT_TRUE(inWindow(parseClock("05:59"), qs, qe));
  TEST_ASSERT_FALSE(inWindow(parseClock("06:00"), qs, qe));
  TEST_ASSERT_FALSE(inWindow(parseClock("12:00"), qs, qe));
  TEST_ASSERT_FALSE(inWindow(parseClock("12:00"), qs, qs));  // start == end: never
  TEST_ASSERT_FALSE(inWindow(parseClock("12:00"), -1, qe));
}

void test_active_profile_weekday_morning() {
  Profile am;
  am.name = "Weekday morning";
  am.days = 0b0111110;  // Mon..Fri
  am.start_min = parseClock("05:30");
  am.end_min = parseClock("10:00");
  Profile pm;
  pm.name = "Weekday evening";
  pm.days = 0b0111110;
  pm.start_min = parseClock("15:00");
  pm.end_min = parseClock("19:30");
  std::vector<Profile> ps = {am, pm};
  TEST_ASSERT_EQUAL_INT(0, activeProfile(ps, 1, parseClock("07:15")));   // Monday
  TEST_ASSERT_EQUAL_INT(1, activeProfile(ps, 3, parseClock("17:00")));   // Wednesday
  TEST_ASSERT_EQUAL_INT(-1, activeProfile(ps, 3, parseClock("12:00")));
  TEST_ASSERT_EQUAL_INT(-1, activeProfile(ps, 0, parseClock("07:15")));  // Sunday
  TEST_ASSERT_EQUAL_INT(-1, activeProfile(ps, 6, parseClock("17:00")));  // Saturday
  TEST_ASSERT_EQUAL_INT(-1, activeProfile({}, 1, parseClock("07:15")));
}

void test_active_profile_crossing_midnight_uses_start_day() {
  Profile late;
  late.name = "Friday night";
  late.days = 1u << 5;  // Friday only
  late.start_min = parseClock("22:00");
  late.end_min = parseClock("02:00");
  std::vector<Profile> ps = {late};
  TEST_ASSERT_EQUAL_INT(0, activeProfile(ps, 5, parseClock("23:00")));   // Friday 23:00
  TEST_ASSERT_EQUAL_INT(0, activeProfile(ps, 6, parseClock("01:00")));   // Saturday 01:00 still Friday's night
  TEST_ASSERT_EQUAL_INT(-1, activeProfile(ps, 6, parseClock("02:00")));
  TEST_ASSERT_EQUAL_INT(-1, activeProfile(ps, 6, parseClock("23:00")));  // Saturday night: not Friday's
  TEST_ASSERT_EQUAL_INT(-1, activeProfile(ps, 5, parseClock("01:00")));  // Friday 01:00 is Thursday's night
}

void test_first_matching_profile_wins() {
  Profile a;
  a.name = "A";
  a.days = 0x7F;
  a.start_min = parseClock("06:00");
  a.end_min = parseClock("12:00");
  Profile b = a;
  b.name = "B";
  b.start_min = parseClock("08:00");
  b.end_min = parseClock("10:00");
  TEST_ASSERT_EQUAL_INT(0, activeProfile({a, b}, 2, parseClock("09:00")));  // both match: first wins
  TEST_ASSERT_EQUAL_INT(0, activeProfile({b, a}, 2, parseClock("09:00")));
  TEST_ASSERT_EQUAL_INT(1, activeProfile({b, a}, 2, parseClock("07:00")));  // only A matches
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_clock);
  RUN_TEST(test_window_same_day_and_crossing_midnight);
  RUN_TEST(test_active_profile_weekday_morning);
  RUN_TEST(test_active_profile_crossing_midnight_uses_start_day);
  RUN_TEST(test_first_matching_profile_wins);
  return UNITY_END();
}
