// Converts the local-time strings SEPTA's JSON APIs embed into epoch seconds, deterministically
// on any host (never relies on the C library's timezone database, `localtime`, `mktime`, or the
// TZ environment variable - none of that is reliably present on ESP32, and we want identical
// results in host tests and on device). See DESIGN.md 5 (timeparse.h) and 6 (device tz field).
//
// This header owns no state and allocates nothing; every function is a pure computation over a
// handful of ints. Memory footprint is effectively zero (no class instances, no heap use).
#pragma once
#include <cstdint>
#include <string>

namespace transit {

using Epoch = int64_t;  // matches transit::Epoch in model.h (unix seconds, UTC)

// Describes one DST transition: the Nth occurrence of `weekday` in `month`, at `hour`:00 local
// standard time. `week` is 1-4 for "the Nth one", or 5 to mean "the last one in the month" (the
// EU convention), so this struct also covers non-US DST rules without code changes.
struct DstTransitionRule {
  uint8_t month;    // 1-12
  uint8_t week;     // 1-4, or 5 for "last"
  uint8_t weekday;  // 0=Sunday .. 6=Saturday
  uint8_t hour;     // 0-23, local time
};

// A POSIX-style timezone rule: a standard-time UTC offset, and - if the zone observes DST - a
// DST UTC offset plus the two transition rules. Seconds are "local minus UTC", i.e. the
// conventional signed UTC offset (US Eastern standard time is -5*3600).
struct TimeZoneRule {
  int32_t std_offset_s;
  bool has_dst;
  int32_t dst_offset_s;
  DstTransitionRule dst_start;  // ignored if !has_dst
  DstTransitionRule dst_end;    // ignored if !has_dst
};

// US Eastern time: EST (UTC-5) / EDT (UTC-4), DST from 02:00 on the second Sunday in March to
// 02:00 on the first Sunday in November (the rule in effect since 2007). This is the only zone
// SEPTA needs; other agencies' `*_source.cpp` can define their own TimeZoneRule the same way.
extern const TimeZoneRule kUsEastern;

// Converts a local wall-clock date/time under `tz` to epoch seconds. Handles the DST/standard
// choice itself by evaluating `tz`'s transition rules for `year`. Does not reject the one
// nonexistent hour skipped every spring or disambiguate the one repeated hour every fall - both
// are resolved by simple date/time comparison against the transition instants, which is exactly
// how most embedded DST implementations behave, and neither edge case affects a transit display
// (they cover one hour, twice a year, and never coincide with a bus at 2 a.m. that matters).
Epoch localToEpoch(int year, int month, int day, int hour, int minute, int second,
                    const TimeZoneRule& tz);

// Parses BusSchedules' `DateCalender` field, e.g. "09/13/26 10:25 pm" (MM/DD/YY, 12-hour clock,
// lowercase am/pm). The 2-digit year is interpreted as 2000+YY. Returns 0 and sets *ok=false
// (if ok is non-null) on any parse failure; never throws.
Epoch parseBusScheduleTime(const std::string& s, bool* ok = nullptr);

// Parses Arrivals' `sched_time`/`depart_time` fields, e.g. "2026-09-13 22:24:00.000"
// (YYYY-MM-DD HH:MM:SS, 24-hour, trailing milliseconds ignored). Returns 0 and sets *ok=false
// (if ok is non-null) on any parse failure; never throws.
Epoch parseArrivalsTime(const std::string& s, bool* ok = nullptr);

}  // namespace transit
