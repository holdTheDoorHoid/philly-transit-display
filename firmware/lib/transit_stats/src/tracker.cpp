#include "transit_stats/tracker.h"

#include "transit_stats/log_window.h"  // localServiceDayNewYork, for service-day headway breaks

namespace transit_stats {

namespace {
constexpr int64_t kGhostFutureThresholdS = 180;   // ">180s in the future" => ghost, not arrive
constexpr int64_t kArriveToleranceS = 120;         // "within ±120s of now" => use predicted as actual
constexpr int64_t kNoShowLateS = 600;              // scheduled departure >10 min in the past
constexpr int64_t kOutageThresholdS = 300;         // poll_ok false for >5 min => outage
constexpr int64_t kHorizonThresholds[4] = {900, 600, 300, 120};
// Longest gap that can still be called a headway. Beyond this the two buses are not consecutive
// vehicles on a running service, they are either side of a service break (DESIGN §9.2), and the
// number would be a fact about the timetable's overnight hole, not about waiting for a bus.
constexpr int64_t kServiceBreakGapS = 4 * 3600;

// Ordered to match seatsLevel()'s 0..5 convention (events.h): empty, open, few, standing, packed,
// full. Converts a stored TrackedTrip::last_seats_level back to its CSV token for an `arrive` row
// (the live transit::Arrival, and so its raw SEPTA string, is gone by the time a trip vanishes).
const char* seatsTokenFromLevel(int8_t level) {
  static constexpr const char* kTokens[6] = {"empty", "open", "few", "standing", "packed", "full"};
  if (level < 0 || level >= 6) return "";
  return kTokens[level];
}

// The time a tracked slot is "about": when the vehicle is expected. Live trips are ranked by
// their prediction, pending scheduled rows by their scheduled time; a live trip with no
// prediction at all counts as immediate, because it is the one thing we certainly cannot defer.
transit::Epoch slotKeyTime(const TrackedTrip& t) {
  if (t.kind == TrackedKind::LiveTrip) return t.last_predicted != 0 ? t.last_predicted : 0;
  return t.last_scheduled;
}

size_t countPendingScheduled(const StopState& st) {
  size_t n = 0;
  for (const auto& t : st.trips) {
    if (t.in_use && t.kind == TrackedKind::ScheduledPending) n++;
  }
  return n;
}
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

TrackedTrip* ArrivalTracker::admitTrip(StopState& st, TrackedKind kind, transit::Epoch key_time,
                                        transit::Epoch now) {
  // (1) Far-future arrivals are deliberately ignored rather than tracked (see tracker.h). A trip
  // beyond the horizon will still be there next poll, when it is closer and worth a slot.
  const int64_t ahead = static_cast<int64_t>(key_time) - static_cast<int64_t>(now);
  if (key_time != 0 && ahead > kAdmitHorizonS) {
    st.counters.dropped_observations++;
    return nullptr;
  }

  // (2) Pending scheduled rows exist only for no-show detection; they get their own small quota so
  // a stop whose schedule lists a dozen upcoming trips cannot starve the live vehicles.
  if (kind == TrackedKind::ScheduledPending &&
      countPendingScheduled(st) >= kMaxPendingScheduledPerStop) {
    TrackedTrip* farthest = nullptr;
    for (auto& t : st.trips) {
      if (!t.in_use || t.kind != TrackedKind::ScheduledPending) continue;
      if (!farthest || slotKeyTime(t) > slotKeyTime(*farthest)) farthest = &t;
    }
    if (!farthest || slotKeyTime(*farthest) <= key_time) {
      st.counters.dropped_observations++;
      return nullptr;
    }
    *farthest = TrackedTrip{};
    st.counters.evicted_trips++;
    return farthest;
  }

  for (auto& t : st.trips) {
    if (!t.in_use) return &t;
  }

  // (3) Full: the slot goes to whichever trip is soonest. Evict the farthest-out entry, and only
  // if it is actually farther than the newcomer -- otherwise refuse the newcomer. That asymmetry
  // is what makes a repeated, unchanged feed stable: nothing is ever evicted for something worse,
  // so the same working set survives poll after poll and no first-sighting `pred` row repeats.
  TrackedTrip* victim = nullptr;
  for (auto& t : st.trips) {
    if (!t.in_use) continue;
    if (!victim) { victim = &t; continue; }
    const transit::Epoch vk = slotKeyTime(*victim), tk = slotKeyTime(t);
    // Ties go to the pending scheduled row: a live vehicle is the better evidence.
    if (tk > vk || (tk == vk && t.kind == TrackedKind::ScheduledPending &&
                    victim->kind == TrackedKind::LiveTrip)) {
      victim = &t;
    }
  }
  if (!victim || slotKeyTime(*victim) <= key_time) {
    st.counters.dropped_observations++;
    return nullptr;
  }
  *victim = TrackedTrip{};
  st.counters.evicted_trips++;
  return victim;
}

void ArrivalTracker::freeTrip(TrackedTrip& t) { t = TrackedTrip{}; }

// ---- public API ---------------------------------------------------------------------------

void ArrivalTracker::registerStop(const std::string& stop_key, const std::string& route,
                                   const std::string& dir) {
  StopState& st = findOrCreateStop(stop_key);
  const bool service_changed = (!st.route.empty() || !st.dir.empty()) &&
                                (st.route != route || st.dir != dir);
  st.route = route;
  st.dir = dir;
  if (service_changed) {
    // Everything tracked belongs to the old route/direction; keeping it would attribute one
    // service's vehicles to another, and would join a headway across the change (F20).
    for (auto& t : st.trips) freeTrip(t);
    st.has_last_arrive = false;
  }
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

  if (!poll_ok) {
    // F17: a failed poll is not an observation. Record that we are blind and change nothing else
    // -- every tracked trip keeps its state, its missed_polls counter does not advance, and no
    // inference of any kind can be drawn from this call.
    handlePollFailure(st, now, out);
    return;
  }

  transit::Epoch outage_start = 0;
  const bool recovered = handlePollRecovery(st, now, outage_start, out);

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

  // Order matters: reconcile AFTER the snapshot has been applied, so a trip that came back is
  // already marked last_seen == now and cannot be closed twice, and BEFORE the normal reap, so a
  // trip closed as "unobserved" is not also reaped as a fresh disappearance.
  if (recovered) reconcileAfterOutage(st, outage_start, now, out);

  reapVanishedAndExpired(st, now, out);
}

// ---- outage ---------------------------------------------------------------------------------

void ArrivalTracker::handlePollFailure(StopState& st, transit::Epoch now,
                                        std::vector<LogEvent>& out) {
  if (!st.poll_failing) {
    st.poll_failing = true;
    st.poll_fail_start = now;
    st.outage_emitted = false;
    // Any blind spell, however short, may hide a bus: the next arrival is not known to be the
    // next consecutive vehicle, so the headway chain stops here (F20).
    st.has_last_arrive = false;
    return;
  }
  if (!st.outage_emitted && (now - st.poll_fail_start) > kOutageThresholdS) {
    st.outage_emitted = true;
    // F23: the row is timestamped with the FIRST failure, not with the moment we crossed the
    // 5-minute threshold -- otherwise a 600 s outage is recorded as 299 s of downtime and every
    // outage total is short by exactly the detection delay. The detection delay itself is kept in
    // horizon_s so the row still says when the device noticed. NOTE for readers of the log: this
    // makes an outage-start row the one kind of row whose ts can be earlier than the row before
    // it in the file.
    LogEvent ev = baseEvent(st, st.poll_fail_start, EventType::Outage);
    ev.horizon_s = static_cast<int32_t>(now - st.poll_fail_start);
    out.push_back(std::move(ev));
  }
}

bool ArrivalTracker::handlePollRecovery(StopState& st, transit::Epoch now,
                                         transit::Epoch& outage_start,
                                         std::vector<LogEvent>& out) {
  if (!st.poll_failing) return false;

  outage_start = st.poll_fail_start;
  if (st.outage_emitted) {
    LogEvent ev = baseEvent(st, now, EventType::Outage);
    ev.note = kNoteOutageEnd;
    out.push_back(std::move(ev));
  }
  st.poll_failing = false;
  st.outage_emitted = false;
  // Even a blip too short to be worth an outage row still needs reconciling: buses passed while
  // we were not looking, and the trips waiting on them must not be mistaken for fresh evidence.
  return true;
}

void ArrivalTracker::reconcileAfterOutage(StopState& st, transit::Epoch outage_start,
                                           transit::Epoch now, std::vector<LogEvent>& out) {
  for (auto& t : st.trips) {
    if (!t.in_use) continue;
    if (t.last_seen >= now) continue;  // present in this fresh snapshot: nothing to reconcile

    if (t.kind == TrackedKind::LiveTrip) {
      // Its predicted time came and went while we were blind. We did not see it arrive and we
      // cannot now: the trip is closed as an explicitly UNOBSERVED passage rather than either
      // fabricated as an ordinary arrival or left to drift until it looks like a ghost.
      if (t.last_predicted == 0 || t.last_predicted > now) continue;

      LogEvent ev = baseEvent(st, now, EventType::Arrive);
      ev.trip = t.trip;
      ev.vehicle = t.vehicle;
      if (t.last_scheduled != 0) ev.scheduled_ts = t.last_scheduled;
      ev.actual_ts = t.last_predicted;  // the only estimate we have; the note says so
      if (t.late_known) ev.late_min = t.last_late_min;
      // Remaining horizon at the moment we lost sight of it -- the same meaning horizon_s has on
      // the other inferred-arrival rows.
      ev.horizon_s = static_cast<int32_t>(static_cast<int64_t>(t.last_predicted) -
                                           static_cast<int64_t>(outage_start));
      ev.seats = seatsTokenFromLevel(t.last_seats_level);
      ev.note = kNoteUnobserved;
      // Deliberately no headway_s: we do not know this bus was the next one, and an unobserved
      // passage does not start a new chain either (st.has_last_arrive was cleared at failure).
      out.push_back(std::move(ev));

      st.counters.arrivals_seen++;
      st.counters.unobserved_arrivals++;
      freeTrip(t);
      continue;
    }

    // ScheduledPending: if its no-show deadline passed while we were blind, we cannot tell "the
    // bus never came" from "we never looked". Drop it silently -- calling that a no-show is
    // exactly the fabrication F18 asks us to stop (the poll-outage rows already record the gap).
    if (now - t.last_scheduled > kNoShowLateS) {
      freeTrip(t);
    }
  }
}

// ---- live arrivals / pred + arrive/ghost -----------------------------------------------------

void ArrivalTracker::retirePendingScheduled(StopState& st, const transit::Arrival& a) {
  // A scheduled trip and the live vehicle running it are the same bus, so the pending record must
  // be retired the moment the vehicle shows up -- otherwise the stop emits an `arrive` for the
  // live trip AND, ten minutes later, a `noshow` for the schedule row it already satisfied (F18).
  //
  // Two ways to recognise it, because SEPTA's static-GTFS trip ids and its realtime trip ids are
  // different namespaces (DESIGN.md §4.4): the ids match, or the scheduled times match. When
  // transit_core adds Arrival::sched_trip (the static id the realtime trip was matched to), add
  // `|| (!a.sched_trip.empty() && t.trip == a.sched_trip)` below -- it is a strictly better join
  // than the time match and needs no other change here.
  for (auto& t : st.trips) {
    if (!t.in_use || t.kind != TrackedKind::ScheduledPending) continue;
    const bool same_id = !a.trip.empty() && t.trip == a.trip;
    const bool same_scheduled = a.scheduled != 0 && t.last_scheduled == a.scheduled;
    if (same_id || same_scheduled) freeTrip(t);
  }
}

void ArrivalTracker::processLiveArrival(StopState& st, const transit::Arrival& a,
                                         transit::Epoch now, std::vector<LogEvent>& out) {
  retirePendingScheduled(st, a);

  TrackedTrip* existing = findTrip(st, a.trip, TrackedKind::LiveTrip);
  const bool is_new = (existing == nullptr);
  if (is_new) {
    existing = admitTrip(st, TrackedKind::LiveTrip, a.predicted, now);
    if (!existing) return;  // beyond the horizon, or every slot holds something sooner
    existing->in_use = true;
    existing->kind = TrackedKind::LiveTrip;
    existing->trip = a.trip;
  }
  TrackedTrip& t = *existing;

  t.vehicle = a.vehicle;
  t.last_predicted = a.predicted;
  t.last_scheduled = a.scheduled;
  t.last_late_min = a.late_min;
  t.late_known = a.late_known;
  t.last_seen = now;
  t.missed_polls = 0;  // present again: the disappearance confirmation count restarts from zero
  t.first_missing_ts = 0;
  t.seq = ++st.touch_seq;
  const char* seats_tok = seatsToken(a.seats);
  t.last_seats_level = static_cast<int8_t>(seatsLevel(seats_tok));

  const int64_t horizon = static_cast<int64_t>(a.predicted) - static_cast<int64_t>(now);

  auto emitPred = [&]() {
    LogEvent ev = baseEvent(st, now, EventType::Pred);
    ev.trip = t.trip;
    ev.vehicle = t.vehicle;
    if (t.last_scheduled != 0) ev.scheduled_ts = t.last_scheduled;
    if (t.last_predicted != 0) ev.predicted_ts = t.last_predicted;
    if (t.late_known) ev.late_min = t.last_late_min;
    ev.horizon_s = static_cast<int32_t>(horizon);
    ev.seats = seats_tok;
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

  // Never open a pending record for a trip we are already watching live: that is the same bus,
  // and the schedule row would only come back later as a spurious no-show (F18, mirror image of
  // retirePendingScheduled()).
  for (const auto& t : st.trips) {
    if (!t.in_use || t.kind != TrackedKind::LiveTrip) continue;
    if ((!a.trip.empty() && t.trip == a.trip) ||
        (a.scheduled != 0 && t.last_scheduled == a.scheduled)) {
      return;
    }
  }

  TrackedTrip* slot = admitTrip(st, TrackedKind::ScheduledPending, a.scheduled, now);
  if (!slot) return;
  slot->in_use = true;
  slot->kind = TrackedKind::ScheduledPending;
  slot->trip = a.trip;
  slot->last_scheduled = a.scheduled;
  slot->last_seen = now;
  slot->seq = ++st.touch_seq;
  slot->route_had_live_through_window = route_has_live_vehicles;
}

// ---- reap: trips no longer present (live) or expired (scheduled) ----------------------------

void ArrivalTracker::reapVanishedAndExpired(StopState& st, transit::Epoch now,
                                             std::vector<LogEvent>& out) {
  // Inferred passages are collected first and emitted in time order, NOT in slot order (F20).
  // When three buses vanish from one observation, the order their slots happen to sit in says
  // nothing about the order they passed the stop; emitting in slot order made last_arrive_actual
  // walk backwards and produced negative headways that the aggregator then counted as bunching.
  std::array<uint8_t, kMaxTrackedTripsPerStop> passages{};
  std::array<transit::Epoch, kMaxTrackedTripsPerStop> passage_ts{};
  size_t passage_count = 0;

  for (size_t i = 0; i < st.trips.size(); i++) {
    TrackedTrip& t = st.trips[i];
    if (!t.in_use) continue;

    if (t.kind == TrackedKind::LiveTrip) {
      if (t.last_seen == now) {  // still present this round
        t.missed_polls = 0;
        t.first_missing_ts = 0;
        continue;
      }

      if (t.missed_polls == 0) t.first_missing_ts = now;
      if (t.missed_polls < 255) t.missed_polls++;
      // One missing observation is routinely a partial feed, not a bus (tracker.h); wait for
      // kMissesBeforeInference consecutive successful polls to agree before concluding anything.
      if (t.missed_polls < kMissesBeforeInference) continue;

      const int64_t diff = static_cast<int64_t>(t.last_predicted) -
                            static_cast<int64_t>(t.first_missing_ts);
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
        freeTrip(t);
        continue;
      }

      passage_ts[passage_count] = (diff >= -kArriveToleranceS && diff <= kArriveToleranceS)
                                       ? t.last_predicted
                                       : t.first_missing_ts;
      passages[passage_count] = static_cast<uint8_t>(i);
      passage_count++;
      continue;
    }

    // ScheduledPending
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
      ev.note = kNoteNoLiveVehicles;
      out.push_back(std::move(ev));
    }
    freeTrip(t);
  }

  // Insertion sort by inferred passage time (at most kMaxTrackedTripsPerStop entries; ties keep
  // slot order, which is stable and so keeps the output reproducible).
  for (size_t i = 1; i < passage_count; i++) {
    const uint8_t idx = passages[i];
    const transit::Epoch ts = passage_ts[i];
    size_t j = i;
    while (j > 0 && passage_ts[j - 1] > ts) {
      passages[j] = passages[j - 1];
      passage_ts[j] = passage_ts[j - 1];
      j--;
    }
    passages[j] = idx;
    passage_ts[j] = ts;
  }

  for (size_t k = 0; k < passage_count; k++) {
    TrackedTrip& t = st.trips[passages[k]];
    const transit::Epoch actual_ts = passage_ts[k];
    const int64_t diff = static_cast<int64_t>(t.last_predicted) -
                          static_cast<int64_t>(t.first_missing_ts);

    LogEvent ev = baseEvent(st, now, EventType::Arrive);
    ev.trip = t.trip;
    ev.vehicle = t.vehicle;
    if (t.last_scheduled != 0) ev.scheduled_ts = t.last_scheduled;
    ev.actual_ts = actual_ts;
    if (t.late_known) ev.late_min = t.last_late_min;
    // How much horizon was left when we lost sight of it: negative means it was still being
    // predicted long after it should have arrived.
    ev.horizon_s = static_cast<int32_t>(diff);
    ev.seats = seatsTokenFromLevel(t.last_seats_level);
    ev.note = (diff < -kArriveToleranceS) ? kNoteLateVanish : kNoteInferred;

    const int64_t day = localServiceDayNewYork(actual_ts);
    if (st.has_last_arrive) {
      const int64_t gap = static_cast<int64_t>(actual_ts) -
                           static_cast<int64_t>(st.last_arrive_actual);
      // A headway is only written when it is a real, forward gap between two consecutive buses on
      // the same service day. Zero or negative gaps (two trips whose inferred times coincide, or
      // a clock that moved) are LEFT UNSET rather than written as 0: an omitted value reads as
      // "unknown" everywhere downstream, while a 0 would be averaged in as a real, perfectly
      // bunched pair (F20).
      if (gap > 0 && gap <= kServiceBreakGapS && day == st.last_arrive_day) {
        ev.headway_s = static_cast<int32_t>(gap);
      }
    }
    out.push_back(std::move(ev));

    st.has_last_arrive = true;
    st.last_arrive_actual = actual_ts;
    st.last_arrive_day = day;
    st.counters.arrivals_seen++;
    st.counters.has_last_late_min = t.late_known;
    st.counters.last_late_min = t.last_late_min;
    freeTrip(t);
  }
}

}  // namespace transit_stats
