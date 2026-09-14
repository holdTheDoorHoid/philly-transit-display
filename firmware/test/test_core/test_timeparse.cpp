#include <unity.h>

#include "transit_core/timeparse.h"

using transit::parseArrivalsTime;
using transit::parseBusScheduleTime;

// Expected epoch values below were independently computed with Python's `zoneinfo` against the
// real IANA America/New_York database (not derived from this project's own DST code), e.g.:
//   python3 -c "from zoneinfo import ZoneInfo; from datetime import datetime;
//     print(int(datetime(2026,9,13,22,25,0,tzinfo=ZoneInfo('America/New_York')).timestamp()))"

void test_bus_schedule_time_edt() {
  bool ok = false;
  transit::Epoch e = parseBusScheduleTime("09/13/26 10:25 pm", &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT64(1789352700, e);
}

void test_bus_schedule_time_est() {
  bool ok = false;
  transit::Epoch e = parseBusScheduleTime("01/15/26 08:00 am", &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT64(1768482000, e);
}

void test_bus_schedule_time_midnight_and_noon_edge() {
  bool ok = false;
  TEST_ASSERT_EQUAL_INT64(1767244500, parseBusScheduleTime("01/01/26 12:15 am", &ok));
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT64(1767287700, parseBusScheduleTime("01/01/26 12:15 pm", &ok));
  TEST_ASSERT_TRUE(ok);
}

void test_bus_schedule_time_next_day_rollover() {
  // BusSchedules emits entries past midnight for the "day" they belong to, e.g. the tail of
  // Sunday night service - the DateCalender field already carries the correct next-day date
  // (this fixture value comes from firmware/test/fixtures/busschedules_21297.json).
  bool ok = false;
  transit::Epoch e = parseBusScheduleTime("09/14/26 12:03 am", &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT64(1789358580, e);
}

void test_bus_schedule_time_invalid_input() {
  bool ok = true;
  transit::Epoch e = parseBusScheduleTime("not a time", &ok);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL_INT64(0, e);

  ok = true;
  e = parseBusScheduleTime("13/40/26 25:99 xm", &ok);
  TEST_ASSERT_FALSE(ok);
}

void test_arrivals_time_edt() {
  bool ok = false;
  transit::Epoch e = parseArrivalsTime("2026-09-13 22:24:00.000", &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT64(1789352640, e);
}

void test_arrivals_time_invalid_input() {
  bool ok = true;
  transit::Epoch e = parseArrivalsTime("garbage", &ok);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL_INT64(0, e);
}

// --- DST boundary: second Sunday in March 2026 is March 8; the clock jumps 02:00 -> 03:00 EDT.

void test_dst_spring_forward_boundary() {
  bool ok = false;
  // Saturday night before the change: still EST (UTC-5).
  TEST_ASSERT_EQUAL_INT64(1772942400, parseBusScheduleTime("03/07/26 11:00 pm", &ok));
  TEST_ASSERT_TRUE(ok);
  // Monday just after midnight, safely past the 2 a.m. transition: EDT (UTC-4).
  TEST_ASSERT_EQUAL_INT64(1773028800, parseBusScheduleTime("03/09/26 12:00 am", &ok));
  TEST_ASSERT_TRUE(ok);
}

// --- DST boundary: first Sunday in November 2026 is November 1; the clock falls back 02:00 ->
// 01:00 EST.

void test_dst_fall_back_boundary() {
  bool ok = false;
  // Saturday night before the change: still EDT (UTC-4).
  TEST_ASSERT_EQUAL_INT64(1793502000, parseBusScheduleTime("10/31/26 11:00 pm", &ok));
  TEST_ASSERT_TRUE(ok);
  // Monday just after midnight, safely past the 2 a.m. transition: EST (UTC-5).
  TEST_ASSERT_EQUAL_INT64(1793595600, parseBusScheduleTime("11/02/26 12:00 am", &ok));
  TEST_ASSERT_TRUE(ok);
}

void test_dst_mid_summer_is_edt() {
  bool ok = false;
  TEST_ASSERT_EQUAL_INT64(1783180800, parseBusScheduleTime("07/04/26 12:00 pm", &ok));
  TEST_ASSERT_TRUE(ok);
}
