#include "daypart_core/daypart.h"

namespace daypart {

int parseClock(const std::string& s) {
  if (s.size() != 5 || s[2] != ':') return -1;
  for (size_t i : {0u, 1u, 3u, 4u}) {
    if (s[i] < '0' || s[i] > '9') return -1;
  }
  int hh = (s[0] - '0') * 10 + (s[1] - '0');
  int mm = (s[3] - '0') * 10 + (s[4] - '0');
  if (hh > 23 || mm > 59) return -1;
  return hh * 60 + mm;
}

bool inWindow(int now_min, int start_min, int end_min) {
  if (start_min < 0 || end_min < 0 || now_min < 0) return false;
  if (start_min == end_min) return false;
  if (start_min < end_min) return now_min >= start_min && now_min < end_min;
  return now_min >= start_min || now_min < end_min;  // crosses midnight
}

int activeProfile(const std::vector<Profile>& profiles, int weekday, int now_min) {
  if (weekday < 0 || weekday > 6) return -1;
  for (size_t i = 0; i < profiles.size(); ++i) {
    const Profile& p = profiles[i];
    if (p.start_min < 0 || p.end_min < 0 || p.start_min == p.end_min) continue;
    bool crosses = p.start_min > p.end_min;
    if (!crosses) {
      if ((p.days & (1u << weekday)) && now_min >= p.start_min && now_min < p.end_min) return (int)i;
      continue;
    }
    // Evening part belongs to today's bit, the after-midnight part to yesterday's bit.
    if (now_min >= p.start_min && (p.days & (1u << weekday))) return (int)i;
    int yesterday = (weekday + 6) % 7;
    if (now_min < p.end_min && (p.days & (1u << yesterday))) return (int)i;
  }
  return -1;
}

}  // namespace daypart
