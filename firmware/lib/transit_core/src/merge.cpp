#include "transit_core/merge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "transit_core/numparse.h"

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

// Largest lateness this project will believe, in minutes. A day's worth is already absurd for a
// city bus; anything beyond it is a malformed status string, not a very late train.
constexpr int kMaxLateMinutes = 1440;

// Parses a SEPTA rail status like "3 min" or "-1 min" into a signed minute count. Returns false
// for anything else ("On Time" is handled by the caller separately; "Delayed"/"Suspended"/etc.
// fall through to false).
//
// The digit run is bounded and the value range-checked BEFORE anything is stored (numparse.h):
// the previous `n = n * 10 + digit` loop had no bound at all, so a status of
// "999999999999999999999999999 min" - which costs SEPTA, or anything sitting between us and
// SEPTA, one JSON string to produce - was signed-overflow undefined behaviour (confirmed with
// UBSan), not merely a wrong number.
bool parseSignedMinutes(const std::string& s, int* out) {
  // Hand-rolled rather than sscanf() (see timeparse.cpp).
  const char* p = s.c_str();
  while (*p == ' ' || *p == '\t') ++p;
  int64_t n = 0;
  // 4 digits is 9999 minutes, comfortably past the +/-1440 bound below; rejecting longer runs is
  // what makes the accumulation itself impossible to overflow.
  if (!parseIntBounded(&p, 4, -kMaxLateMinutes, kMaxLateMinutes, &n)) return false;
  if (*p != ' ' && *p != '\t') return false;  // a unit must follow: "3min" is not SEPTA's shape
  while (*p == ' ' || *p == '\t') ++p;
  std::string u;
  while (*p && *p != ' ' && *p != '\t' && u.size() < 15) u.push_back(*p++);
  if (u.empty()) return false;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p != '\0') return false;  // trailing garbage: "3 min later" is not a minute count
  for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (u == "min" || u == "mins" || u == "minute" || u == "minutes") {
    *out = static_cast<int>(n);
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

// Configured subway route id -> the route ids BusSchedules actually keys that service under.
// Verified live 2026-09-13 against google_bus.zip's routes.txt and a real BusSchedules response
// for Snyder (stop 1286), which came back as {"B1": [...]} for a station configured as "BSL" -
// see NOTES.md 7a. B1/B2/B3 are Broad St Local / Express / Broad-Ridge Spur; L1 is the
// Market-Frankford line, all stops.
struct SubwaySchedAlias {
  const char* cfg_route;
  const char* sched_routes[4];  // nullptr-terminated
};
const SubwaySchedAlias kSubwaySchedAliases[] = {
    {"BSL", {"B1", "B2", "B3", nullptr}},
    {"MFL", {"L1", "L2", nullptr}},
};

}  // namespace

bool schedRouteMatches(const StopConfig& cfg, const SchedEntry& entry) {
  if (cfg.route.empty()) return true;  // no route filter configured
  if (equalsIgnoreCase(cfg.route, entry.route)) return true;

  if (cfg.mode != Mode::Subway) {
    // Bus, trolley (and rail, which never reaches here): the id spaces are the same one, so
    // anything else at this shared stop belongs to a different route's panel.
    return false;
  }

  for (const auto& alias : kSubwaySchedAliases) {
    if (!equalsIgnoreCase(cfg.route, alias.cfg_route)) continue;
    for (int i = 0; i < 4 && alias.sched_routes[i] != nullptr; ++i) {
      if (equalsIgnoreCase(entry.route, alias.sched_routes[i])) return true;
    }
    return false;  // known subway id, and this entry is not one of its GTFS ids
  }

  // An unlisted subway route id (a line whose GTFS ids this project has not verified - NHSL, the
  // owl variants BSO/MFO, anything SEPTA renames). Accept whatever BusSchedules returned for the
  // station: the alternative is a blank panel, and unlike a shared bus stop, a subway station's
  // stop_id serves one line, so there is no other route's schedule here to confuse it with.
  // Deliberately narrow: this fallback applies to Mode::Subway only.
  return true;
}

namespace {

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

// Index of the unconsumed schedule entry nearest to `anchor` within +/-600s (DESIGN.md 7), or -1.
int nearestSchedule(const StopConfig& cfg, const std::vector<SchedEntry>& sched,
                     const std::vector<bool>& consumed, Epoch anchor) {
  int best = -1;
  Epoch best_delta = 601;  // > 600s means "no match" (10 minute cap)
  for (size_t i = 0; i < sched.size(); ++i) {
    if (consumed[i]) continue;
    if (!stringMatches(cfg.direction, sched[i].direction)) continue;
    if (!schedRouteMatches(cfg, sched[i])) continue;
    Epoch delta = sched[i].scheduled > anchor ? sched[i].scheduled - anchor
                                               : anchor - sched[i].scheduled;
    if (delta <= 600 && delta < best_delta) {
      best_delta = delta;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// Index of the soonest still-upcoming unconsumed schedule entry, or -1. Used to place a SKIPPED
// update that carries no time of its own: GTFS-RT gives nothing to join on in that case (its
// trip ids are a different id space from BusSchedules' static ones, DESIGN.md 4.4), so the best
// available reading of "this trip's next call here is cancelled" is the next scheduled call that
// no live vehicle has already claimed. Running this only after every timed update has matched is
// what makes that a reasonable guess rather than a coin toss.
int soonestUnconsumed(const StopConfig& cfg, const std::vector<SchedEntry>& sched,
                       const std::vector<bool>& consumed, Epoch now) {
  int best = -1;
  for (size_t i = 0; i < sched.size(); ++i) {
    if (consumed[i]) continue;
    if (!stringMatches(cfg.direction, sched[i].direction)) continue;
    if (!schedRouteMatches(cfg, sched[i])) continue;
    if (sched[i].scheduled <= now - 60) continue;
    if (best < 0 || sched[i].scheduled < sched[static_cast<size_t>(best)].scheduled) {
      best = static_cast<int>(i);
    }
  }
  return best;
}

// Plausible epoch floor for a timestamp SEPTA reports: TransitView pairs its "no GPS fix"
// vehicles with values like 63240 (NOTES.md 3), which is not a time at all.
constexpr Epoch kPlausibleEpochFloor = 1000000000;  // 2001-09-09

}  // namespace

StopSnapshot mergeStop(const StopConfig& cfg, const std::vector<StopTimeUpdate>& rt,
                        const std::vector<TvVehicle>& tv, const std::vector<SchedEntry>& sched,
                        Epoch now, const SourceStatus& sources) {
  StopSnapshot snap;
  snap.key = cfg.key;
  snap.fetched = now;

  std::vector<bool> sched_consumed(sched.size(), false);
  Epoch feed_ts = 0;         // newest realtime feed timestamp seen for this stop, 0 = unknown
  Epoch vehicle_ts = 0;      // newest plausible TransitView vehicle timestamp, same convention
  bool any_live_row = false;  // any row that came from a realtime prediction (Live or Skipped)

  // Pass 1: every realtime update that carries a time. Doing the timed ones first means they get
  // first claim on the schedule entries, and a timeless SKIPPED row (pass 2) can only take one
  // nothing else wanted.
  std::vector<size_t> skipped_without_time;
  for (size_t ri = 0; ri < rt.size(); ++ri) {
    const auto& u = rt[ri];
    if (u.stop_id != cfg.stop_id) continue;
    if (!cfg.route.empty() && u.route_id != cfg.route) continue;
    if (!directionMatchesInt(cfg.direction, u.direction_id)) continue;

    if (u.feed_timestamp > feed_ts) feed_ts = u.feed_timestamp;

    // NO_DATA is the feed saying "I have nothing for this stop", which is not a prediction and
    // must not become a zero-time row or an invented one - leaving the schedule entry unconsumed
    // is exactly right, since the schedule is then the best information available.
    if (u.stopNoData()) continue;

    Epoch predicted = u.predictedTime();

    if (u.tripCanceled()) {
      // The trip is not running. Emit nothing, but claim the schedule entry it would have been
      // matched to, so the fallback below cannot re-advertise it as an ordinary scheduled
      // arrival. Without a time there is nothing to claim (see merge.h).
      if (predicted > 0) {
        int idx = nearestSchedule(cfg, sched, sched_consumed, predicted);
        if (idx >= 0) sched_consumed[static_cast<size_t>(idx)] = true;
      }
      continue;
    }

    if (u.stopSkipped() && predicted == 0) {
      skipped_without_time.push_back(ri);
      continue;
    }
    if (predicted == 0) continue;  // SCHEDULED/UNSCHEDULED with no time: nothing to say

    const TvVehicle* match = findTvByTrip(tv, u.trip_id);
    // SEPTA uses implausible large `late` values (998/999) as "no live tracking" sentinels
    // (see septa.h TvVehicle::late); treat those the same as "no match".
    bool late_known = (match != nullptr) && (match->late > -900 && match->late < 900);
    int late_min = late_known ? match->late : 0;
    if (match != nullptr && match->timestamp >= kPlausibleEpochFloor &&
        match->timestamp > vehicle_ts) {
      vehicle_ts = match->timestamp;
    }

    Arrival a;
    a.trip = u.trip_id;
    a.vehicle = (match && !match->vehicle_id.empty()) ? match->vehicle_id : u.vehicle_id;
    a.destination = (match && !match->destination.empty()) ? match->destination : cfg.headsign;
    a.predicted = predicted;
    a.late_min = static_cast<int16_t>(late_min);
    a.late_known = late_known;
    a.status = u.stopSkipped() ? Status::Skipped : Status::Live;
    a.stop_sequence = static_cast<uint16_t>(u.stop_sequence);
    a.seats = match ? match->seats : std::string();

    Epoch anchor = a.predicted - (late_known ? static_cast<Epoch>(late_min) * 60 : 0);
    int best = nearestSchedule(cfg, sched, sched_consumed, anchor);
    if (best >= 0) {
      a.scheduled = sched[static_cast<size_t>(best)].scheduled;
      a.sched_trip = sched[static_cast<size_t>(best)].trip_id;
      sched_consumed[static_cast<size_t>(best)] = true;
    }

    // A Skipped row is realtime-derived too, so it counts as "the live source is working".
    any_live_row = true;
    snap.arrivals.push_back(std::move(a));
  }

  // Pass 2: SKIPPED updates with no time. These used to become zero-time rows that the staleness
  // filter deleted, after which the fallback below printed their scheduled counterpart as a
  // perfectly ordinary upcoming bus - the detour disappeared and was replaced by a promise.
  for (size_t ri : skipped_without_time) {
    const auto& u = rt[ri];
    int idx = soonestUnconsumed(cfg, sched, sched_consumed, now);
    if (idx < 0) continue;  // nothing to attach it to, and no time of its own: not displayable
    const SchedEntry& e = sched[static_cast<size_t>(idx)];
    sched_consumed[static_cast<size_t>(idx)] = true;

    const TvVehicle* match = findTvByTrip(tv, u.trip_id);
    Arrival a;
    a.trip = u.trip_id;
    a.vehicle = (match && !match->vehicle_id.empty()) ? match->vehicle_id : u.vehicle_id;
    a.destination = (match && !match->destination.empty())
                         ? match->destination
                         : (!e.direction_desc.empty() ? e.direction_desc : cfg.headsign);
    a.scheduled = e.scheduled;       // the only time this row has; predicted stays 0
    a.sched_trip = e.trip_id;
    a.status = Status::Skipped;
    a.stop_sequence = static_cast<uint16_t>(u.stop_sequence);
    a.seats = match ? match->seats : std::string();
    any_live_row = true;
    snap.arrivals.push_back(std::move(a));
  }

  // Unmatched, still-relevant schedule entries become their own Scheduled rows. For a
  // Subway-mode stop (empty rt/tv passed in by the caller - see source.h), this is the *only*
  // loop that runs, which is what makes subway schedule-only display fall out of this function
  // for free rather than needing a special case.
  for (size_t i = 0; i < sched.size(); ++i) {
    if (sched_consumed[i]) continue;
    if (!stringMatches(cfg.direction, sched[i].direction)) continue;
    if (!schedRouteMatches(cfg, sched[i])) continue;
    if (sched[i].scheduled <= now - 60) continue;

    // One row per static trip id. SEPTA's wrong-service-day answers (septa_source.h,
    // fetchPlausibleSchedule) have been seen listing the same trip on three consecutive days;
    // keep whichever copy comes first.
    bool duplicate = false;
    for (auto& existing : snap.arrivals) {
      if (existing.status != Status::Scheduled || existing.trip != sched[i].trip_id) continue;
      if (sched[i].scheduled < existing.scheduled) existing.scheduled = sched[i].scheduled;
      duplicate = true;
      break;
    }
    if (duplicate) continue;

    Arrival a;
    a.trip = sched[i].trip_id;
    a.destination = !sched[i].direction_desc.empty() ? sched[i].direction_desc : cfg.headsign;
    a.scheduled = sched[i].scheduled;
    a.sched_trip = sched[i].trip_id;  // a schedule-only row IS its static trip
    a.status = Status::Scheduled;
    snap.arrivals.push_back(std::move(a));
  }

  // --- Feed age (DESIGN.md 4.7) --------------------------------------------------------------
  // GTFS-RT's header timestamp is the only thing that distinguishes a live feed from a cached or
  // replayed one; the fetch succeeding says nothing about it. A missing timestamp (0) is "unknown
  // age", and unknown is left alone rather than guessed at in either direction.
  snap.source_ts = feed_ts != 0 ? feed_ts : (vehicle_ts != 0 ? vehicle_ts : now);
  Epoch judged_ts = feed_ts != 0 ? feed_ts : vehicle_ts;
  bool stale_feed = judged_ts != 0 && (judged_ts < now - kFeedStaleAfterS ||
                                        judged_ts > now + kFeedFutureToleranceS);
  if (stale_feed) {
    // Demote everything realtime-derived: an old feed's predictions are worse than useless (they
    // look current), but its matched scheduled times are still real scheduled times.
    for (auto& a : snap.arrivals) {
      if (a.status != Status::Live && a.status != Status::Skipped) continue;
      if (a.scheduled > 0) {
        a.predicted = 0;
        a.late_known = false;
        a.late_min = 0;
        a.status = Status::Scheduled;
      } else {
        a.predicted = 0;  // effective() becomes 0, so sortAndDropStale() drops the row
        a.scheduled = 0;
      }
    }
    any_live_row = false;
  }

  sortAndDropStale(&snap, now);

  // --- Per-stop health (DESIGN.md 7) ---------------------------------------------------------
  // Which sources this stop actually needs follows its mode; a subway stop has no realtime source
  // to fail, so a TripUpdates outage must not mark it down, and equally a BusSchedules failure
  // for a subway stop is total rather than cosmetic.
  bool needs_live = (cfg.mode == Mode::Bus || cfg.mode == Mode::Trolley);
  bool live_ok = !needs_live || sources.live_ok;
  bool vehicles_ok = !needs_live || sources.vehicles_ok;
  bool sources_ok = live_ok && vehicles_ok && sources.schedule_ok;

  if (stale_feed) {
    snap.health = Health::Stale;
    snap.error = "live feed stale";
  } else if (!sources_ok && snap.arrivals.empty()) {
    snap.health = Health::Unavailable;
  } else if (any_live_row) {
    snap.health = Health::Live;
  } else {
    snap.health = Health::ScheduleOnly;
  }

  if (snap.error.empty()) {
    if (!live_ok) {
      snap.error = "live feed unavailable";
    } else if (!sources.schedule_ok) {
      snap.error = "SEPTA schedule unavailable";
    } else if (!vehicles_ok) {
      snap.error = "vehicle positions unavailable";
    }
  }
  snap.ok = sources_ok && snap.health != Health::Stale && snap.health != Health::Unavailable;
  if (snap.ok) snap.error.clear();
  return snap;
}

StopSnapshot mergeRail(const StopConfig& cfg, const std::vector<RailArrival>& rail, Epoch now,
                        const SourceStatus& sources) {
  StopSnapshot snap;
  snap.key = cfg.key;
  snap.fetched = now;
  // Arrivals is generated per request and publishes no "produced at" field, so the fetch time is
  // the best honest answer for when this data was made (see merge.h).
  snap.source_ts = now;

  bool have_line_filter = !cfg.route.empty();
  // Accepts the line code or the display name, so a config still carrying "Paoli/Thorndale"
  // rather than "PAO" keeps working (septa.h findRailLineByName).
  const RailLine* line = have_line_filter ? findRailLineByName(cfg.route) : nullptr;

  for (const auto& r : rail) {
    if (!stringMatches(cfg.direction, r.direction)) continue;
    if (have_line_filter) {
      // An unrecognized line code matches nothing, rather than silently passing every line
      // through - see merge.h.
      if (line == nullptr || !equalsIgnoreCase(r.line, line->display_name)) continue;
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

  snap.ok = sources.live_ok;
  if (!snap.ok) {
    snap.health = Health::Unavailable;
    snap.error = "SEPTA rail arrivals unavailable";
  } else {
    // An Arrivals response with no trains is a real answer (late night, or a station that is
    // between trains), not a degraded one - so this stays Live rather than becoming a fake
    // "schedule only", which would be a claim about a source Regional Rail does not have here.
    snap.health = Health::Live;
  }
  return snap;
}

}  // namespace transit
