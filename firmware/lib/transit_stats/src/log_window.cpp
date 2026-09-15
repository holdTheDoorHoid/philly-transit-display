#include "transit_stats/log_window.h"

#include <cstdio>

namespace transit_stats {

namespace {

// Same deterministic, OS-timezone-independent approach as aggregate.cpp's
// americaNewYorkLocalHourWeekday() (Howard Hinnant's civil-calendar algorithms; see that file's
// own comment for why this project never uses the C library's tz database). Reimplemented here
// in miniature rather than shared across the library boundary, matching the existing convention:
// transit_core/timeparse.cpp and transit_stats/aggregate.cpp each already keep their own small
// private copy of this same day-counting arithmetic for their own purposes.

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) q--;
  return q;
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int64_t &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yr = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = yr + (m <= 2);
}

int weekdayFromDays(int64_t days) { return static_cast<int>(((days % 7) + 7 + 4) % 7); }

int64_t nthSundayOfMonth(int64_t year, unsigned month, int n) {
  int64_t day1 = daysFromCivil(year, month, 1);
  int w = weekdayFromDays(day1);
  int firstSundayDom = 1 + ((7 - w) % 7);
  int sundayDom = firstSundayDom + (n - 1) * 7;
  return daysFromCivil(year, month, static_cast<unsigned>(sundayDom));
}

// UTC epoch -> local (year, month), America/New_York, current US DST rule.
void localYearMonth(transit::Epoch utc, int &year, int &month) {
  const int64_t localDays = localServiceDayNewYork(utc);
  int64_t ly;
  unsigned lm, ld;
  civilFromDays(localDays, ly, lm, ld);
  year = static_cast<int>(ly);
  month = static_cast<int>(lm);
}

}  // namespace

int64_t localServiceDayNewYork(transit::Epoch utc) {
  const int64_t days = floorDiv(utc, 86400);
  int64_t y;
  unsigned mo, da;
  civilFromDays(days, y, mo, da);
  (void)mo;
  (void)da;

  const int64_t marchSecondSunday = nthSundayOfMonth(y, 3, 2);
  const int64_t novFirstSunday = nthSundayOfMonth(y, 11, 1);
  const transit::Epoch dstStartUtc = marchSecondSunday * 86400 + 7 * 3600;  // 02:00 EST = 07:00 UTC
  const transit::Epoch dstEndUtc = novFirstSunday * 86400 + 6 * 3600;       // 02:00 EDT = 06:00 UTC
  const bool isDst = (utc >= dstStartUtc) && (utc < dstEndUtc);
  const int64_t offsetSeconds = isDst ? -4 * 3600 : -5 * 3600;

  return floorDiv(static_cast<int64_t>(utc) + offsetSeconds, 86400);
}

std::vector<std::string> monthsInWindow(transit::Epoch window_start, transit::Epoch window_end) {
  std::vector<std::string> out;
  if (window_end <= window_start) return out;

  int y0 = 0, m0 = 0, y1 = 0, m1 = 0;
  localYearMonth(window_start, y0, m0);
  localYearMonth(window_end - 1, y1, m1);

  int y = y0, m = m0;
  for (int guard = 0; guard < 1200; ++guard) {  // 100 years - a hard safety cap, never hit in practice
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d-%02d", y, m);
    out.emplace_back(buf);
    if (y == y1 && m == m1) break;
    if (++m > 12) {
      m = 1;
      ++y;
    }
  }
  return out;
}

}  // namespace transit_stats
