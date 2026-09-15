// Checked hand-rolled integer/float parsing for transit_core. Arduino-independent (host +
// ESP32); header-only, allocation-free, no exceptions.
//
// Why this exists: every parser in this library reads numbers out of strings SEPTA (or anyone
// upstream of it) controls, and the obvious `n = n * 10 + (c - '0')` loop is undefined behaviour
// the moment the input has enough digits - signed overflow, not a wrapped value. A 27-digit
// "999999999999999999999999999 min" in a rail status string was enough to trip UBSan in
// merge.cpp's parseSignedMinutes(). The C library's atoi/atol/atoll/atof are no better: they are
// explicitly UB on out-of-range input.
//
// The rule these helpers follow: bound the DIGIT COUNT before accumulating (so the accumulator
// can never leave int64 range at all), then range-check the value, then narrow. Callers pass the
// tightest practical bounds they can justify rather than the type's limits.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>

namespace transit {

// Widest digit count that cannot overflow int64 (10^18 - 1 < INT64_MAX).
constexpr int kMaxSafeDigits = 18;

// Parses an optionally-signed decimal integer at *p, advancing *p past it on success.
// Does NOT skip leading whitespace (callers that allow it skip it themselves, so that "  - 5"
// stays a parse error rather than becoming -5).
//
// Fails - leaving *p and *out untouched - if there are no digits, if there are more than
// `max_digits` of them (which is how overflow is made impossible rather than merely detected), or
// if the value falls outside [min_value, max_value]. `max_digits` above kMaxSafeDigits is clamped.
inline bool parseIntBounded(const char** p, int max_digits, int64_t min_value, int64_t max_value,
                             int64_t* out) {
  if (p == nullptr || *p == nullptr || out == nullptr) return false;
  if (max_digits > kMaxSafeDigits) max_digits = kMaxSafeDigits;
  if (max_digits < 1) return false;

  const char* q = *p;
  bool neg = false;
  if (*q == '-' || *q == '+') {
    neg = (*q == '-');
    ++q;
  }
  int64_t n = 0;
  int digits = 0;
  while (*q >= '0' && *q <= '9') {
    if (digits == max_digits) return false;  // too long: reject rather than truncate or wrap
    n = n * 10 + (*q - '0');                 // safe: digits < max_digits <= 18
    ++q;
    ++digits;
  }
  if (digits == 0) return false;
  if (neg) n = -n;
  if (n < min_value || n > max_value) return false;
  *p = q;
  *out = n;
  return true;
}

// Whole-string variant: the entire string must be one integer, apart from optional surrounding
// spaces/tabs. Same bounding rules as parseIntBounded(); trailing garbage is a failure.
inline bool parseIntStrict(const std::string& s, int max_digits, int64_t min_value,
                            int64_t max_value, int64_t* out) {
  const char* p = s.c_str();
  while (*p == ' ' || *p == '\t') ++p;
  int64_t v = 0;
  if (!parseIntBounded(&p, max_digits, min_value, max_value, &v)) return false;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p != '\0') return false;
  *out = v;
  return true;
}

// Whole-string double, for the two coordinate fields SEPTA quotes as strings. strtod (not atof:
// atof is UB out of range) plus an explicit "something was consumed and only spaces follow"
// check, so "39.95abc" is a failure rather than 39.95. Out-of-range input makes strtod return
// +/-HUGE_VAL, which is finite-but-huge rather than UB; the caller's own sanity checks (a
// latitude is +/-90) catch that, and treating it as a parse failure here would silently zero a
// coordinate that was merely written oddly.
inline bool parseDoubleStrict(const std::string& s, double* out) {
  const char* p = s.c_str();
  while (*p == ' ' || *p == '\t') ++p;
  char* end = nullptr;
  double v = std::strtod(p, &end);
  if (end == p) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  *out = v;
  return true;
}

}  // namespace transit
