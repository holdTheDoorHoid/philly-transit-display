// ArrivalTracker: turns a stream of transit::StopSnapshot observations into LogEvent rows,
// per DESIGN.md §9.1. Arduino-independent, no exceptions, no RTTI, no heap growth beyond the
// fixed caps described below.
//
// The one idea the rest of this header follows from: an arrival is never observed, only INFERRED
// from a prediction that stopped being published. That makes two things mandatory.
//   * A failed poll is not evidence (F17). While polling for a stop is failing, the tracker
//     freezes that stop: no arrive, ghost, noshow or headway can be produced by a network
//     timeout. Inference resumes only from fresh, successful observations.
//   * Every inference says how it was made: the `note` column on an arrive row carries
//     kNoteInferred / kNoteLateVanish / kNoteUnobserved (events.h), so the stats layer can report
//     "N inferred arrivals" instead of presenting a guess as a measurement (F22).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "transit_core/model.h"
#include "transit_stats/events.h"

namespace transit_stats {

// Hard caps, per DESIGN's "never load a month into memory" / bounded-heap requirement.
// FOUR since 0.3.2-rc2, tracking config_store.h's kMaxStops, which became 4 in the same release
// (the owner's "a maximum of four stops", and what LVGL's 36 KB pool can actually draw). This is
// the single largest heap object on the device that is sized off the stop count, and it is
// heap-allocated once at boot (net_poller.cpp preallocateTracker()).
//
// WHAT IT GIVES BACK, read out of the image's DWARF rather than estimated: sizeof(StopState) is
// **1,056 B** on the ESP32 and sizeof(ArrivalTracker) is **4,232 B** at four slots, so the array is
// the whole object bar 8 bytes and eight slots would be 8,456 B. Halving it returns **4,224 B**.
//
// Stated plainly because a larger figure was expected: 4,224 B is LESS than the 8,064 B that
// 0.3.2-rc1 made resident to keep a poll cycle's contiguous demand down, so this does not pay for
// that on its own - see DESIGN.md SS5 for the full ledger, .bss included. rc1 was measured taking
// the resting floor to ~23 KB and driving min_free8 to 156 B on the owner's board, which is what
// this release is trying to undo.
constexpr size_t kMaxTrackedStops = 4;          // matches config.json's 4-stop maximum (§6)
// 8, not 12, since 0.3.1 (owner decision, 2026-09-17). F19 had raised it from 8 to 12 to stop an
// ordinary feed churning its slots; this gives ~4,000 B of the ESP32's heap back - 8 stops x 4
// slots x ~124 B - on a board where the largest free block, not the free heap, is the resource
// that runs out (DESIGN.md SS12.1). What it costs is stated rather than hidden: a stop whose feed
// carries more than eight upcoming trips at once now drops the farthest ones, and if the set
// churns across polls the tracker can emit a fresh first-sighting `pred` row for a trip it had
// already seen. The owner's stops carry 4-8, so this does not bite there; a busier stop would
// show it as extra `pred` rows in the CSV, never as a wrong arrival.
constexpr size_t kMaxTrackedTripsPerStop = 8;   // live trips + pending scheduled trips, combined

// ---- admission / retention policy (F19) -------------------------------------------------------
//
// Slots are not a cache of "whatever arrived most recently": they are a working set of the trips
// this stop actually needs, and the policy below is what stops an ordinary nine- or twenty-trip
// feed from evicting the entry it is about to need again and re-emitting a first-sighting `pred`
// row for it every poll.
//   * Nothing more than kAdmitHorizonS away is admitted at all. A bus 90 minutes out tells the
//     user nothing and its prediction will be rewritten many times before it matters; admitting
//     it would cost a slot that a bus 4 minutes away needs.
//   * When every slot is full, the candidate evicted is the one whose time is FARTHEST out, and
//     only if it is farther than the incoming one. So the set converges on the soonest arrivals
//     and then stops changing -- an unchanged feed produces no churn at all.
//   * Pending scheduled entries are capped separately (kMaxPendingScheduledPerStop) so schedule
//     rows, which exist only for no-show detection, can never crowd out live vehicles.
// Whatever is not admitted is counted in StopCounters::dropped_observations rather than silently
// discarded, so the device can say it is dropping data instead of quietly under-reporting.
constexpr int64_t kAdmitHorizonS = 45 * 60;
constexpr size_t kMaxPendingScheduledPerStop = 3;

// How many CONSECUTIVE successful observations must fail to mention a tracked trip before its
// disappearance is treated as evidence of anything (F17). One missing observation is routinely
// just a partial GTFS-RT feed or an entity that exceeded GTFSRT_MAX_ENTITY (DESIGN.md §4.2); two
// in a row, with a successful poll each time, is the feed actually saying the trip is gone. The
// cost of the extra confirmation is one poll cycle of latency on every arrive/ghost row.
constexpr uint8_t kMissesBeforeInference = 2;

// Kind of thing a tracked-trip slot holds.
enum class TrackedKind : uint8_t { LiveTrip, ScheduledPending };

// One tracked trip (live or scheduled-only) for one configured stop.
// sizeof(TrackedTrip) is 128 B measured on the 64-bit host and ~112 B on the 32-bit ESP32 (two
// std::string members, mostly short SEPTA ids that fit in small-string-optimization and so cost
// no heap: 32 B each on the host, 24 B on target; four int64 epochs; a handful of int32/bool
// fields, plus one int8_t for last_seats_level and one uint8_t for missed_polls).
struct TrackedTrip {
  bool in_use = false;
  TrackedKind kind = TrackedKind::LiveTrip;
  std::string trip;
  std::string vehicle;
  transit::Epoch last_predicted = 0;  // last Arrival::predicted seen (LiveTrip)
  transit::Epoch last_scheduled = 0;  // last Arrival::scheduled seen (either kind)
  transit::Epoch last_seen = 0;       // observe() timestamp this trip was last present
  int32_t last_late_min = 0;
  bool late_known = false;
  // Which "ETA dropped under N seconds" pred milestones have already been emitted for this trip.
  bool pred_emitted_900 = false;
  bool pred_emitted_600 = false;
  bool pred_emitted_300 = false;
  bool pred_emitted_120 = false;
  // ScheduledPending only: whether route_has_live_vehicles has been true at every observe() call
  // since this trip started being tracked (see noshow-vs-outage rule below).
  bool route_had_live_through_window = true;
  uint32_t seq = 0;  // per-stop touch counter, kept for diagnostics and stable eviction tie-breaks
  // Last seen seatsLevel() (0=empty..5=full, -1 unknown), LiveTrip only. Kept so the `arrive` row
  // emitted after the trip vanishes (transit::Arrival is gone by then) can still carry a crowding
  // token, converted back to its string form at emit time (see tracker.cpp).
  int8_t last_seats_level = -1;
  // Consecutive SUCCESSFUL observations of this stop that did not mention this trip, and the
  // timestamp of the first of them. first_missing_ts -- not "now" -- is the best estimate of when
  // the vehicle passed, so it is what the arrive/ghost decision and actual_ts are measured
  // against; using "now" would make the inferred time drift by kMissesBeforeInference polls.
  uint8_t missed_polls = 0;
  transit::Epoch first_missing_ts = 0;
};

// Per-stop counters exposed for the on-device stats summary (DESIGN §8 "Stats page").
struct StopCounters {
  uint32_t arrivals_seen = 0;
  uint32_t ghosts_seen = 0;
  bool has_last_late_min = false;
  int32_t last_late_min = 0;
  uint32_t evicted_trips = 0;          // trips dropped from this stop to make room for a sooner one
  uint32_t dropped_observations = 0;   // arrivals never admitted (too far out, or no slot to spare)
  uint32_t unobserved_arrivals = 0;    // arrive rows closed after an outage (note="unobserved")
};

// One configured stop's tracking state.
// sizeof(StopState) is dominated by trips[kMaxTrackedTripsPerStop]: 8 * 128 B = 1024 B measured
// on the host (8 * ~112 B ~= 0.9 KB on target), plus three std::strings and the outage/headway
// scalars. At 12 slots the measured totals were sizeof(StopState) = 1720 B and
// sizeof(ArrivalTracker) = 13768 B on the 64-bit host (~12,040 B on the 32-bit target, measured);
// at 8 they are 4 x 128 B and 8 x 4 x 128 B smaller -- comfortably inside the ~20 KB
// budget the F19 review set, and asserted in test_stats/test_main.cpp. That is
// intentionally *not* held to the StatsAggregator's <8 KB rule: it is a long-lived singleton (one
// instance for the process lifetime), not something instantiated per web request.
struct StopState {
  bool in_use = false;
  std::string stop_key;
  std::string route;
  std::string dir;
  std::array<TrackedTrip, kMaxTrackedTripsPerStop> trips{};
  uint32_t touch_seq = 0;   // monotonic, used to stamp TrackedTrip::seq
  uint32_t last_touch = 0;  // stamped with the tracker's global touch counter on every observe()

  // Poll-outage state (DESIGN §9.1 "outage").
  bool poll_failing = false;
  bool outage_emitted = false;
  transit::Epoch poll_fail_start = 0;

  // Headway state: gap since the previous `arrive` for this stop (direction is fixed per
  // configured stop, so "same stop and direction" reduces to "same stop_key"). Continuity is
  // DELIBERATELY broken -- has_last_arrive goes false -- by anything that means "we may have
  // missed a bus in between": any poll failure, a local service-day boundary, a gap longer than
  // kServiceBreakGapS, an arrival we only inferred after an outage, and re-registering the stop
  // onto a different route/direction. A headway is a claim about two consecutive buses; if we
  // cannot see that they were consecutive, we do not make the claim (F20).
  bool has_last_arrive = false;
  transit::Epoch last_arrive_actual = 0;
  int64_t last_arrive_day = 0;  // localServiceDayNewYork(last_arrive_actual)

  StopCounters counters;
};

// ArrivalTracker: one instance owns state for up to kMaxTrackedStops configured stops.
//
// Usage: call registerStop() once per configured stop (ideally at startup, from StopConfig) so
// LogEvent::route/dir are populated; then call observe() once per poll cycle per configured
// stop's StopSnapshot. observe() never blocks, allocates only via the fixed internal arrays
// (aside from the std::string copies of ids, which are typically SSO and heap-free), and appends
// zero or more LogEvents to `out` (never clears `out` itself, so callers can batch across stops).
class ArrivalTracker {
 public:
  // Registers (or updates) the route/direction label used when logging events for `stop_key`.
  // Safe to call multiple times (e.g. on config change). If never called for a stop_key before
  // observe() sees it, that stop's LogEvents simply have empty route/dir columns.
  //
  // Re-registering a stop onto a DIFFERENT route/direction discards everything tracked for it and
  // breaks headway continuity: the buses before and after such a change are not consecutive
  // vehicles on one service, so no gap between them means anything (F20).
  void registerStop(const std::string& stop_key, const std::string& route, const std::string& dir);

  // Consumes one StopSnapshot for one configured stop and appends any LogEvents it produces to
  // `out`. See DESIGN.md §9.1 for the event rules; the exact tie-breaking rules this
  // implementation uses are documented on the corresponding private helpers in tracker.cpp.
  //
  //   snap                     current arrivals for this configured stop (snap.key identifies it)
  //   now                      current time (unix seconds, UTC)
  //   route_has_live_vehicles  true if this stop's route currently has >=1 realtime-tracked
  //                            vehicle anywhere (used for the noshow-vs-outage rule)
  //   poll_ok                  false if the most recent network poll for this stop's data failed.
  //                            THIS IS NOT A SNAPSHOT OF REALITY: when it is false, `snap` says
  //                            nothing about the world, so observe() updates outage bookkeeping
  //                            and returns without touching a single tracked trip (F17). An empty
  //                            snapshot with poll_ok=false can never produce an arrive, ghost,
  //                            noshow or headway. Callers must pass the honest value -- passing
  //                            true for a failed fetch reintroduces the defect.
  //   out                      LogEvents are appended (not cleared) here, in the order produced
  void observe(const transit::StopSnapshot& snap, transit::Epoch now, bool route_has_live_vehicles,
               bool poll_ok, std::vector<LogEvent>& out);

  // Returns the current counters for `stop_key`, or false if that stop is not currently tracked
  // (never observed, or evicted by the kMaxTrackedStops cap).
  bool getStopCounters(const std::string& stop_key, StopCounters& out) const;

  // Number of distinct stops currently tracked (<= kMaxTrackedStops).
  uint8_t trackedStopCount() const;

  // Number of stops that have been dropped entirely because more than kMaxTrackedStops distinct
  // stop_keys were observed (least-recently-touched stop evicted each time).
  uint32_t evictedStopCount() const { return evicted_stop_count_; }

 private:
  std::array<StopState, kMaxTrackedStops> stops_{};
  uint32_t global_touch_seq_ = 0;
  uint32_t evicted_stop_count_ = 0;

  StopState* findStop(const std::string& key);
  StopState& findOrCreateStop(const std::string& key);

  static TrackedTrip* findTrip(StopState& st, const std::string& trip_id, TrackedKind kind);
  // Admission/retention policy (see the constants above). Returns nullptr when the arrival must
  // not be tracked at all, having counted it in dropped_observations.
  static TrackedTrip* admitTrip(StopState& st, TrackedKind kind, transit::Epoch key_time,
                                 transit::Epoch now);
  static void freeTrip(TrackedTrip& t);

  // Poll-failure bookkeeping. handlePollFailure() freezes the stop; handlePollRecovery() closes
  // the outage and reports whether this call is the first successful one after a failure.
  void handlePollFailure(StopState& st, transit::Epoch now, std::vector<LogEvent>& out);
  bool handlePollRecovery(StopState& st, transit::Epoch now, transit::Epoch& outage_start,
                           std::vector<LogEvent>& out);
  // After an outage: close trips whose predicted time passed while we were blind as "unobserved"
  // passages, and quietly drop pending scheduled rows we can no longer judge (F17, F18).
  void reconcileAfterOutage(StopState& st, transit::Epoch outage_start, transit::Epoch now,
                             std::vector<LogEvent>& out);
  void processLiveArrival(StopState& st, const transit::Arrival& a, transit::Epoch now,
                           std::vector<LogEvent>& out);
  void processScheduledArrival(StopState& st, const transit::Arrival& a, transit::Epoch now,
                                bool route_has_live_vehicles, std::vector<LogEvent>& out);
  // Retires the pending scheduled record that this live arrival turns out to BE (F18).
  static void retirePendingScheduled(StopState& st, const transit::Arrival& a);
  void reapVanishedAndExpired(StopState& st, transit::Epoch now, std::vector<LogEvent>& out);

  LogEvent baseEvent(const StopState& st, transit::Epoch now, EventType type) const;
};

}  // namespace transit_stats
