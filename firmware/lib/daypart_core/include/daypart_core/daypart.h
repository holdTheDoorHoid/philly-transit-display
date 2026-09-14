// Time-of-day windows for quiet hours and commute profiles (DESIGN.md SS6 "quiet", "profiles").
// Arduino-independent; works in minutes-since-local-midnight so callers pass whatever clock they
// have (localtime_r on the device, fixed numbers in tests).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace daypart {

// "HH:MM" -> minutes since midnight; -1 if malformed or out of range.
int parseClock(const std::string& hhmm);

// True when `now_min` falls inside [start_min, end_min). A window whose end is at or before its
// start crosses midnight ("23:00".."06:00"); start == end is treated as "never".
bool inWindow(int now_min, int start_min, int end_min);

struct Profile {
  std::string name;
  uint8_t days = 0;   // bit 0 = Sunday .. bit 6 = Saturday
  int start_min = 0;
  int end_min = 0;
};

// Index of the first profile active at `weekday` (0 = Sunday) and `now_min`, or -1. For a window
// that crosses midnight, the *start* day's bit is the one that matters: a Friday 22:00-02:00
// profile is still active at Saturday 01:00.
int activeProfile(const std::vector<Profile>& profiles, int weekday, int now_min);

}  // namespace daypart
