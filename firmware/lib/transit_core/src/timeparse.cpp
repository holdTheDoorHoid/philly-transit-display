#include "transit_core/timeparse.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "transit_core/numparse.h"

namespace transit {

namespace {

bool isLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

int daysInMonth(int year, int month) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) return 29;
  return kDays[month - 1];
}

// Days since 1970-01-01 for a proleptic-Gregorian civil date. Howard Hinnant's well-known
// branch-free algorithm (http://howardhinnant.github.io/date_algorithms.html); deliberately not
// using the C library's calendar functions, which depend on the host's notion of `time_t` range
// and timezone state and would not give identical results on ESP32 vs. host tests.
int64_t daysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int yoe = static_cast<int>(y - era * 400);                      // [0, 399]
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;        // [0, 365]
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                 // [0, 146096]
  return era * 146097 + doe - 719468;
}

// 0=Sunday..6=Saturday for a given days-since-epoch value. 1970-01-01 (z=0) was a Thursday.
int weekdayOf(int64_t z) {
  return static_cast<int>(((z % 7) + 7 + 4) % 7);
}

// Day-of-month (1-based) of the transition described by `rule`, for `year`.
int nthWeekdayOfMonth(int year, const DstTransitionRule& rule) {
  int64_t first = daysFromCivil(year, rule.month, 1);
  int first_dow = weekdayOf(first);
  if (rule.week == 5) {  // last occurrence in the month
    int last_day = daysInMonth(year, rule.month);
    int64_t last = daysFromCivil(year, rule.month, last_day);
    int last_dow = weekdayOf(last);
    int back = (last_dow - rule.weekday + 7) % 7;
    return last_day - back;
  }
  int offset = (rule.weekday - first_dow + 7) % 7;
  return 1 + offset + (rule.week - 1) * 7;
}

// "Naive" epoch (wall-clock seconds since 1970-01-01T00:00:00, ignoring any UTC offset) of the
// instant a DST transition rule describes, for `year`.
int64_t transitionNaiveEpoch(int year, const DstTransitionRule& rule) {
  int day = nthWeekdayOfMonth(year, rule);
  int64_t days = daysFromCivil(year, rule.month, day);
  return days * 86400 + static_cast<int64_t>(rule.hour) * 3600;
}

// Reads an unsigned run of at most `max_digits` decimal digits at *p into `out`; advances p.
// False if there are none, if there are MORE than max_digits (a date field with too many digits
// is malformed input, not a field to silently truncate - see numparse.h for why every number
// parser in this library bounds the digit count rather than the accumulated value), or if the
// value somehow leaves int range. No sign is accepted: none of these fields is ever signed, and
// letting "-" through here would make "09/-1/26" parse.
bool readInt(const char*& p, int& out, int max_digits) {
  if (*p == '-' || *p == '+') return false;
  int64_t v = 0;
  if (!parseIntBounded(&p, max_digits, 0, 999999, &v)) return false;
  out = static_cast<int>(v);
  return true;
}

bool expect(const char*& p, char c) {
  if (*p != c) return false;
  ++p;
  return true;
}

void skipSpaces(const char*& p) {
  while (*p == ' ' || *p == '\t') ++p;
}

}  // namespace

const TimeZoneRule kUsEastern = {
    -5 * 3600,       // EST
    true,
    -4 * 3600,       // EDT
    {3, 2, 0, 2},    // second Sunday in March, 02:00
    {11, 1, 0, 2},   // first Sunday in November, 02:00
};

Epoch localToEpoch(int year, int month, int day, int hour, int minute, int second,
                    const TimeZoneRule& tz) {
  int64_t days = daysFromCivil(year, month, day);
  int64_t naive = days * 86400 + static_cast<int64_t>(hour) * 3600 +
                   static_cast<int64_t>(minute) * 60 + second;

  int32_t offset = tz.std_offset_s;
  if (tz.has_dst) {
    int64_t dst_start = transitionNaiveEpoch(year, tz.dst_start);
    int64_t dst_end = transitionNaiveEpoch(year, tz.dst_end);
    if (naive >= dst_start && naive < dst_end) {
      offset = tz.dst_offset_s;
    }
  }
  return static_cast<Epoch>(naive - offset);
}

// Hand-rolled rather than sscanf(): newlib's scanf family costs ~15 KB of flash on the ESP32 and
// these are the only callers in the firmware.
Epoch parseBusScheduleTime(const std::string& s, bool* ok) {
  return parseBusScheduleTime(s.c_str(), ok);
}

Epoch parseBusScheduleTime(const char* s, bool* ok) {
  if (s == nullptr) {
    if (ok) *ok = false;
    return 0;
  }
  int mm = 0, dd = 0, yy = 0, hh = 0, mi = 0;
  const char* p = s;
  skipSpaces(p);
  bool good = readInt(p, mm, 2) && expect(p, '/') && readInt(p, dd, 2) && expect(p, '/') && readInt(p, yy, 4);
  if (good) {
    skipSpaces(p);
    good = readInt(p, hh, 2) && expect(p, ':') && readInt(p, mi, 2);
  }
  if (good) skipSpaces(p);
  if (!good || mm < 1 || mm > 12 || dd < 1 || dd > 31 || hh < 1 || hh > 12 || mi < 0 || mi > 59) {
    if (ok) *ok = false;
    return 0;
  }
  char a = static_cast<char>(std::tolower(static_cast<unsigned char>(p[0])));
  char m = p[0] ? static_cast<char>(std::tolower(static_cast<unsigned char>(p[1]))) : '\0';
  bool is_pm;
  if (a == 'p' && m == 'm') {
    is_pm = true;
  } else if (a == 'a' && m == 'm') {
    is_pm = false;
  } else {
    if (ok) *ok = false;
    return 0;
  }
  int hour24 = hh % 12;  // 12am -> 0, 12pm -> 12
  if (is_pm) hour24 += 12;
  int year = 2000 + yy;

  if (ok) *ok = true;
  return localToEpoch(year, mm, dd, hour24, mi, 0, kUsEastern);
}

Epoch parseArrivalsTime(const std::string& s, bool* ok) {
  int year = 0, month = 0, day = 0, hh = 0, mi = 0, se = 0;
  const char* p = s.c_str();
  skipSpaces(p);
  bool good = readInt(p, year, 4) && expect(p, '-') && readInt(p, month, 2) && expect(p, '-') && readInt(p, day, 2);
  if (good) {
    skipSpaces(p);
    good = readInt(p, hh, 2) && expect(p, ':') && readInt(p, mi, 2) && expect(p, ':') && readInt(p, se, 2);
  }
  if (!good || month < 1 || month > 12 || day < 1 || day > 31 || hh < 0 || hh > 23 || mi < 0 ||
      mi > 59 || se < 0 || se > 60) {
    if (ok) *ok = false;
    return 0;
  }
  if (ok) *ok = true;
  return localToEpoch(year, month, day, hh, mi, se, kUsEastern);
}

}  // namespace transit
