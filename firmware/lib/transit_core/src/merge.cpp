#include "transit_core/merge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace transit {

namespace {

std::string trimWs(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Parses a SEPTA rail status like "3 min" or "-1 min" into a signed minute count. Returns false
// for anything else ("On Time" is handled by the caller separately; "Delayed"/"Suspended"/etc.
// fall through to false).
bool parseSignedMinutes(const std::string& s, int* out) {
  int n = 0;
  char unit[16] = {0};
  if (std::sscanf(s.c_str(), "%d %15s", &n, unit) != 2) return false;
  std::string u(unit);
  for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (u == "min" || u == "mins" || u == "minute" || u == "minutes") {
    *out = n;
    return true;
  }
  return false;
}

// Direction match against a GTFS direction_id (bus/trolley/subway realtime side). An empty
// cfg.direction means "both directions"; an unknown feed direction_id (-1) is let through since
// StopTimeUpdate::stop_id has already scoped this to the physically-correct side of the stop in
// practice (see merge.h's header comment).
bool directionMatchesInt(const std::string& cfg_direction, int rt_direction_id) {
  if (cfg_direction.empty()) return true;
  if (rt_direction_id < 0) return true;
  return std::atoi(cfg_direction.c_str()) == rt_direction_id;
}

// Direction/line-style string match (BusSchedules' "0"/"1", or rail's "N"/"S"). An empty
// cfg_value means "no filter".
bool stringMatches(const std::string& cfg_value, const std::string& other) {
  if (cfg_value.empty()) return true;
  return cfg_value == other;
}

const TvVehicle* findTvByTrip(const std::vector<TvVehicle>& tv, const std::string& trip_id) {
  for (const auto& v : tv) {
    if (v.trip == trip_id) return &v;
  }
  return nullptr;
}

void sortAndDropStale(StopSnapshot* snap, Epoch now) {
  std::sort(snap->arrivals.begin(), snap->arrivals.end(),
            [](const Arrival& x, const Arrival& y) { return x.effective() < y.effective(); });
  snap->arrivals.erase(std::remove_if(snap->arrivals.begin(), snap->arrivals.end(),
                                       [now](const Arrival& a) { return a.effective() < now - 60; }),
                        snap->arrivals.end());
}

}  // namespace

StopSnapshot mergeStop(const StopConfig& cfg, const std::vector<StopTimeUpdate>& rt,
                        const std::vector<TvVehicle>& tv, const std::vector<SchedEntry>& sched,
                        Epoch now) {
  StopSnapshot snap;
  snap.key = cfg.key;
  snap.fetched = now;
  snap.ok = true;

  std::vector<bool> sched_consumed(sched.size(), false);

  for (const auto& u : rt) {
    if (u.stop_id != cfg.stop_id) continue;
    if (!cfg.route.empty() && u.route_id != cfg.route) continue;
    if (!directionMatchesInt(cfg.direction, u.direction_id)) continue;

    const TvVehicle* match = findTvByTrip(tv, u.trip_id);
    // SEPTA uses implausible large `late` values (998/999) as "no live tracking" sentinels
    // (see septa.h TvVehicle::late); treat those the same as "no match".
    bool late_known = (match != nullptr) && (match->late > -900 && match->late < 900);
    int late_min = late_known ? match->late : 0;

    Arrival a;
    a.trip = u.trip_id;
    a.vehicle = (match && !match->vehicle_id.empty()) ? match->vehicle_id : u.vehicle_id;
    a.destination = (match && !match->destination.empty()) ? match->destination : cfg.headsign;
    a.predicted = u.has_arrival_time ? u.arrival_time : (u.has_departure_time ? u.departure_time : 0);
    a.late_min = static_cast<int16_t>(late_min);
    a.late_known = late_known;
    a.status = (u.schedule_relationship == 1 /* SKIPPED */) ? Status::Skipped : Status::Live;
    a.stop_sequence = static_cast<uint16_t>(u.stop_sequence);
    a.seats = match ? match->seats : std::string();

    if (a.predicted > 0) {
      Epoch anchor = a.predicted - (late_known ? static_cast<Epoch>(late_min) * 60 : 0);
      int best = -1;
      Epoch best_delta = 601;  // > 600s means "no match" (10 minute cap, DESIGN.md 7)
      for (size_t i = 0; i < sched.size(); ++i) {
        if (sched_consumed[i]) continue;
        if (!stringMatches(cfg.direction, sched[i].direction)) continue;
        Epoch delta = sched[i].scheduled > anchor ? sched[i].scheduled - anchor
                                                   : anchor - sched[i].scheduled;
        if (delta <= 600 && delta < best_delta) {
          best_delta = delta;
          best = static_cast<int>(i);
        }
      }
      if (best >= 0) {
        a.scheduled = sched[static_cast<size_t>(best)].scheduled;
        sched_consumed[static_cast<size_t>(best)] = true;
      }
    }

    snap.arrivals.push_back(std::move(a));
  }

  // Unmatched, still-relevant schedule entries become their own Scheduled rows. For a
  // Subway-mode stop (empty rt/tv passed in by the caller - see source.h), this is the *only*
  // loop that runs, which is what makes subway schedule-only display fall out of this function
  // for free rather than needing a special case.
  for (size_t i = 0; i < sched.size(); ++i) {
    if (sched_consumed[i]) continue;
    if (!stringMatches(cfg.direction, sched[i].direction)) continue;
    if (sched[i].scheduled <= now - 60) continue;

    Arrival a;
    a.trip = sched[i].trip_id;
    a.destination = !sched[i].direction_desc.empty() ? sched[i].direction_desc : cfg.headsign;
    a.scheduled = sched[i].scheduled;
    a.status = Status::Scheduled;
    snap.arrivals.push_back(std::move(a));
  }

  sortAndDropStale(&snap, now);
  return snap;
}

StopSnapshot mergeRail(const StopConfig& cfg, const std::vector<RailArrival>& rail, Epoch now) {
  StopSnapshot snap;
  snap.key = cfg.key;
  snap.fetched = now;
  snap.ok = true;

  bool have_line_filter = !cfg.route.empty();
  const RailLine* line = have_line_filter ? findRailLine(cfg.route) : nullptr;

  for (const auto& r : rail) {
    if (!stringMatches(cfg.direction, r.direction)) continue;
    if (have_line_filter) {
      // An unrecognized line code matches nothing, rather than silently passing every line
      // through - see merge.h.
      if (line == nullptr || r.line != line->display_name) continue;
    }

    Arrival a;
    a.trip = r.train_id;
    a.destination = !r.destination.empty() ? r.destination : cfg.headsign;
    a.predicted = r.depart;
    a.scheduled = r.sched;
    a.status = Status::Live;

    std::string status = trimWs(r.status);
    int minutes = 0;
    if (equalsIgnoreCase(status, "on time")) {
      a.late_known = true;
      a.late_min = 0;
    } else if (parseSignedMinutes(status, &minutes)) {
      a.late_known = true;
      a.late_min = static_cast<int16_t>(minutes);
    } else {
      a.late_known = false;
      a.late_min = 0;
    }

    snap.arrivals.push_back(std::move(a));
  }

  sortAndDropStale(&snap, now);
  return snap;
}

}  // namespace transit
