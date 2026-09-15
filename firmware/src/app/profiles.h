// Commute profiles (DESIGN.md SS6 "profiles"): which configured stops the main page shows right
// now. Shared by the UI (which rebuilds when the answer changes) and the web server (which reports
// it in /api/state).
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "config_store.h"
#include "daypart_core/daypart.h"

namespace transit_app {

// Index into cfg.profiles of the active profile at `now`, or -1 (no profile: show every stop).
inline int activeProfileIndex(const Config &cfg, time_t now) {
  if (cfg.profiles.empty() || now < 1700000000) return -1;  // no profiles, or clock not synced
  struct tm lt;
  localtime_r(&now, &lt);
  std::vector<daypart::Profile> ps;
  ps.reserve(cfg.profiles.size());
  for (const ProfileConfig &p : cfg.profiles) {
    daypart::Profile d;
    d.name = p.name;
    d.days = p.days;
    d.start_min = daypart::parseClock(p.start);
    d.end_min = daypart::parseClock(p.end);
    ps.push_back(d);
  }
  return daypart::activeProfile(ps, lt.tm_wday, lt.tm_hour * 60 + lt.tm_min);
}

// The stops the main page should show, in order: the active profile's list, else every stop.
//
// De-duplicates defensively (review F07). jsonToConfig() rejects a profile that lists the same
// key twice, but this runs against whatever is in memory - including a config written by an older
// firmware that had no such check - and a repeated key would build two panels for one stop, each
// with its own LVGL object tree, halving the room the other stops get for no reason the owner
// could diagnose from the screen.
inline std::vector<transit::StopConfig> visibleStops(const Config &cfg, time_t now) {
  int idx = activeProfileIndex(cfg, now);
  if (idx < 0) return cfg.stops;
  std::vector<transit::StopConfig> out;
  for (const std::string &key : cfg.profiles[(size_t)idx].stops) {
    bool already = false;
    for (const transit::StopConfig &o : out) {
      if (o.key == key) {
        already = true;
        break;
      }
    }
    if (already) continue;
    for (const transit::StopConfig &s : cfg.stops) {
      if (s.key == key) {
        out.push_back(s);
        break;
      }
    }
  }
  return out.empty() ? cfg.stops : out;
}

}  // namespace transit_app
