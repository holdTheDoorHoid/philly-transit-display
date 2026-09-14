#include "transit_stats/tracker.h"

namespace transit_stats {

namespace {
constexpr int64_t kGhostFutureThresholdS = 180;   // ">180s in the future" => ghost, not arrive
constexpr int64_t kArriveToleranceS = 120;         // "within ±120s of now" => use predicted as actual
constexpr int64_t kNoShowLateS = 600;              // scheduled departure >10 min in the past
constexpr int64_t kOutageThresholdS = 300;         // poll_ok false for >5 min => outage
constexpr int64_t kHorizonThresholds[4] = {900, 600, 300, 120};
}  // namespace

// ---- lookups / allocation ----------------------------------------------------------------

StopState* ArrivalTracker::findStop(const std::string& key) {
  for (auto& s : stops_) {
    if (s.in_use && s.stop_key == key) return &s;
  }
  return nullptr;
}

StopState& ArrivalTracker::findOrCreateStop(const std::string& key) {
  if (StopState* s = findStop(key)) return *s;

  for (auto& s : stops_) {
    if (!s.in_use) {
      s = StopState{};
      s.in_use = true;
      s.stop_key = key;
      return s;
    }
  }

  // All kMaxTrackedStops slots are in use and none matches `key`: evict the least-recently
  // touched stop (drop-oldest, per DESIGN's bounded-memory requirement).
  StopState* victim = &stops_[0];
  for (auto& s : stops_) {
    if (s.last_touch < victim->last_touch) victim = &s;
  }
  *victim = StopState{};
  victim->in_use = true;
  victim->stop_key = key;
  evicted_stop_count_++;
  return *victim;
}

TrackedTrip* ArrivalTracker::findTrip(StopState& st, const std::string& trip_id, TrackedKind kind) {
  for (auto& t : st.trips) {
    if (t.in_use && t.kind == kind && t.trip == trip_id) return &t;
  }
  return nullptr;
}

TrackedTrip& ArrivalTracker::allocTrip(StopState& st) {
  for (auto& t : st.trips) {
    if (!t.in_use) return t;
  }
  // Evict the least-recently-touched trip in this stop (drop-oldest).
  TrackedTrip* victim = &st.trips[0];
  for (auto& t : st.trips) {
    if (t.seq < victim->seq) victim = &t;
  }
  *victim = TrackedTrip{};
  st.counters.evicted_trips++;
  return *victim;
}

void ArrivalTracker::freeTrip(TrackedTrip& t) { t = TrackedTrip{}; }

// ---- public API ---------------------------------------------------------------------------

void ArrivalTracker::registerStop(const std::string& stop_key, const std::string& route,
                                   const std::string& dir) {
  StopState& st = findOrCreateStop(stop_key);
  st.route = route;
  st.dir = dir;
}

bool ArrivalTracker::getStopCounters(const std::string& stop_key, StopCounters& out) const {
  for (const auto& s : stops_) {
    if (s.in_use && s.stop_key == stop_key) {
      out = s.counters;
      return true;
    }
  }
  return false;
}

uint8_t ArrivalTracker::trackedStopCount() const {
  uint8_t n = 0;
  for (const auto& s : stops_) {
    if (s.in_use) n++;
  }
  return n;
}

LogEvent ArrivalTracker::baseEvent(const StopState& st, transit::Epoch now, EventType type) const {
  LogEvent ev;
  ev.ts = now;
  ev.event = type;
  ev.stop_key = st.stop_key;
  ev.route = st.route;
  ev.dir = st.dir;
  return ev;
}

void ArrivalTracker::observe(const transit::StopSnapshot& snap, transit::Epoch now,
                              bool route_has_live_vehicles, bool poll_ok,
                              std::vector<LogEvent>& out) {
  StopState& st = findOrCreateStop(snap.key);
  st.last_touch = ++global_touch_seq_;

  handlePollOutage(st, now, poll_ok, out);

  // route_has_live_vehicles is a fact about "now"; fold it into every currently-pending
  // scheduled trip's "was there ever a gap with no live vehicle" tracking, regardless of
  // whether that particular trip happens to appear in this snapshot.
  for (auto& t : st.trips) {
    if (t.in_use && t.kind == TrackedKind::ScheduledPending) {
      t.route_had_live_through_window = t.route_had_live_through_window && route_has_live_vehicles;
    }
  }

  for (const auto& a : snap.arrivals) {
    if (a.status == transit::Status::Live) processLiveArrival(st, a, now, out);
  }
  for (const auto& a : snap.arrivals) {
    if (a.status == transit::Status::Scheduled) {
      processScheduledArrival(st, a, now, route_has_live_vehicles, out);
    }
  }

  reapVanishedAndExpired(st, now, out);
}

// ---- outage ---------------------------------------------------------------------------------

void ArrivalTracker::handlePollOutage(StopState& st, transit::Epoch now, bool poll_ok,
                                       std::vector<LogEvent>& out) {
  if (!poll_ok) {
    if (!st.poll_failing) {
      st.poll_failing = true;
      st.poll_fail_start = now;
      st.outage_emitted = false;
    } else if (!st.outage_emitted && (now - st.poll_fail_start) > kOutageThresholdS) {
      st.outage_emitted = true;
      out.push_back(baseEvent(st, now, EventType::Outage));
    }
  } else {
    if (st.poll_failing && st.outage_emitted) {
      LogEvent ev = baseEvent(st, now, EventType::Outage);
      ev.note = "end";
      out.push_back(std::move(ev));
    }
    st.poll_failing = false;
    st.outage_emitted = false;
  }
}

// ---- live arrivals / pred + arrive/ghost -----------------------------------------------------

void ArrivalTracker::processLiveArrival(StopState& st, const transit::Arrival& a,
                                         transit::Epoch now, std::vector<LogEvent>& out) {
  TrackedTrip* existing = findTrip(st, a.trip, TrackedKind::LiveTrip);
  bool is_new = (existing == nullptr);
  TrackedTrip& t = is_new ? allocTrip(st) : *existing;

  if (is_new) {
    t.in_use = true;
    t.kind = TrackedKind::LiveTrip;
    t.trip = a.trip;
  }
  t.vehicle = a.vehicle;
  t.last_predicted = a.predicted;
  t.last_scheduled = a.scheduled;
  t.last_late_min = a.late_min;
  t.late_known = a.late_known;
  t.last_seen = now;
  t.seq = ++st.touch_seq;

  const int64_t horizon = static_cast<int64_t>(a.predicted) - static_cast<int64_t>(now);

  auto emitPred = [&]() {
    LogEvent ev = baseEvent(st, now, EventType::Pred);
    ev.trip = t.trip;
    ev.vehicle = t.vehicle;
    if (t.last_scheduled != 0) ev.scheduled_ts = t.last_scheduled;
    if (t.last_predicted != 0) ev.predicted_ts = t.last_predicted;
    if (t.late_known) ev.late_min = t.last_late_min;
    ev.horizon_s = static_cast<int32_t>(horizon);
    out.push_back(std::move(ev));
  };

  if (is_new) {
    // First sighting: one row, and any threshold already below the current horizon is
    // considered "already reported" by this very row (no separate duplicate row for it).
    emitPred();
    if (horizon < kHorizonThresholds[0]) t.pred_emitted_900 = true;
    if (horizon < kHorizonThresholds[1]) t.pred_emitted_600 = true;
    if (horizon < kHorizonThresholds[2]) t.pred_emitted_300 = true;
    if (horizon < kHorizonThresholds[3]) t.pred_emitted_120 = true;
  } else {
    if (!t.pred_emitted_900 && horizon < kHorizonThresholds[0]) { t.pred_emitted_900 = true; emitPred(); }
    if (!t.pred_emitted_600 && horizon < kHorizonThresholds[1]) { t.pred_emitted_600 = true; emitPred(); }
    if (!t.pred_emitted_300 && horizon < kHorizonThresholds[2]) { t.pred_emitted_300 = true; emitPred(); }
    if (!t.pred_emitted_120 && horizon < kHorizonThresholds[3]) { t.pred_emitted_120 = true; emitPred(); }
  }
}

void ArrivalTracker::processScheduledArrival(StopState& st, const transit::Arrival& a,
                                              transit::Epoch now, bool route_has_live_vehicles,
                                              std::vector<LogEvent>& out) {
  (void)out;  // scheduled sightings never emit a row by themselves; only their expiry does
  TrackedTrip* existing = findTrip(st, a.trip, TrackedKind::ScheduledPending);
  if (existing) {
    existing->last_scheduled = a.scheduled;
    existing->last_seen = now;
    existing->seq = ++st.touch_seq;
    existing->route_had_live_through_window =
        existing->route_had_live_through_window && route_has_live_vehicles;
    return;
  }
  TrackedTrip& t = allocTrip(st);
  t.in_use = true;
  t.kind = TrackedKind::ScheduledPending;
  t.trip = a.trip;
  t.last_scheduled = a.scheduled;
  t.last_seen = now;
  t.seq = ++st.touch_seq;
  t.route_had_live_through_window = route_has_live_vehicles;
}

// ---- reap: trips no longer present (live) or expired (scheduled) ----------------------------

void ArrivalTracker::reapVanishedAndExpired(StopState& st, transit::Epoch now,
                                             std::vector<LogEvent>& out) {
  for (auto& t : st.trips) {
    if (!t.in_use) continue;

    if (t.kind == TrackedKind::LiveTrip) {
      if (t.last_seen == now) continue;  // still present this round

      const int64_t diff = static_cast<int64_t>(t.last_predicted) - static_cast<int64_t>(now);
      if (diff > kGhostFutureThresholdS) {
        LogEvent ev = baseEvent(st, now, EventType::Ghost);
        ev.trip = t.trip;
        ev.vehicle = t.vehicle;
        if (t.last_scheduled != 0) ev.scheduled_ts = t.last_scheduled;
        if (t.last_predicted != 0) ev.predicted_ts = t.last_predicted;
        if (t.late_known) ev.late_min = t.last_late_min;
        ev.horizon_s = static_cast<int32_t>(diff);
        out.push_back(std::move(ev));
        st.counters.ghosts_seen++;
      } else {
        const transit::Epoch actual_ts =
            (diff >= -kArriveToleranceS && diff <= kArriveToleranceS) ? t.last_predicted : now;

        LogEvent ev = baseEvent(st, now, EventType::Arrive);
        ev.trip = t.trip;
        ev.vehicle = t.vehicle;
        if (t.last_scheduled != 0) ev.scheduled_ts = t.last_scheduled;
        ev.actual_ts = actual_ts;
        if (t.late_known) ev.late_min = t.last_late_min;
        if (st.has_last_arrive) {
          ev.headway_s = static_cast<int32_t>(actual_ts - st.last_arrive_actual);
        }
        out.push_back(std::move(ev));

        st.has_last_arrive = true;
        st.last_arrive_actual = actual_ts;
        st.counters.arrivals_seen++;
        st.counters.has_last_late_min = t.late_known;
        st.counters.last_late_min = t.last_late_min;
      }
      freeTrip(t);

    } else {  // ScheduledPending
      if (now - t.last_scheduled <= kNoShowLateS) continue;  // not late enough yet

      if (t.route_had_live_through_window) {
        LogEvent ev = baseEvent(st, now, EventType::NoShow);
        ev.trip = t.trip;
        ev.scheduled_ts = t.last_scheduled;
        out.push_back(std::move(ev));
      } else {
        // Folded into `outage`: the route never showed a live vehicle during the wait, so we
        // cannot distinguish "bus never came" from "we just never observed it". This is a
        // separate concept from the poll-outage start/end pair above (that one is paired by an
        // empty vs "end" note and produces outage_min in StatsAggregator); this one is a single
        // informational row, tagged with note="no_live_vehicles" so it is never mistaken for
        // either end of a poll-outage pair.
        LogEvent ev = baseEvent(st, now, EventType::Outage);
        ev.trip = t.trip;
        ev.scheduled_ts = t.last_scheduled;
        ev.note = "no_live_vehicles";
        out.push_back(std::move(ev));
      }
      freeTrip(t);
    }
  }
}

}  // namespace transit_stats
