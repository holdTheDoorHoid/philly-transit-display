// ArrivalTracker: turns a stream of transit::StopSnapshot observations into LogEvent rows,
// per DESIGN.md §9.1. Arduino-independent, no exceptions, no RTTI, no heap growth beyond the
// fixed caps described below.
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
constexpr size_t kMaxTrackedStops = 8;          // matches config.json's 8-stop maximum (§6)
constexpr size_t kMaxTrackedTripsPerStop = 8;  // live trips + pending scheduled trips, combined (16 cost ~4 KB more RAM)

// Kind of thing a tracked-trip slot holds.
enum class TrackedKind : uint8_t { LiveTrip, ScheduledPending };

// One tracked trip (live or scheduled-only) for one configured stop.
// sizeof(TrackedTrip) is roughly 90-110 bytes (two std::string members, mostly short SEPTA ids
// that fit in small-string-optimization and so cost no heap; a handful of int64/int32/bool
// fields). With kMaxTrackedTripsPerStop = 16 that is under ~2 KB per stop.
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
  uint32_t seq = 0;  // per-stop touch counter, used to evict the least-recently-touched trip
};

// Per-stop counters exposed for the on-device stats summary (DESIGN §8 "Stats page").
struct StopCounters {
  uint32_t arrivals_seen = 0;
  uint32_t ghosts_seen = 0;
  bool has_last_late_min = false;
  int32_t last_late_min = 0;
  uint32_t evicted_trips = 0;  // trips dropped from this stop by the kMaxTrackedTripsPerStop cap
};

// One configured stop's tracking state.
// sizeof(StopState) is dominated by trips[16] (~1.5-1.8 KB); with 8 stops (kMaxTrackedStops),
// ArrivalTracker as a whole is on the order of 14-16 KB. That is intentionally *not* held to the
// StatsAggregator's <8 KB rule: it is a long-lived singleton (one instance for the process
// lifetime), not something instantiated per web request, and its size is fixed at compile time.
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
  // configured stop, so "same stop and direction" reduces to "same stop_key").
  bool has_last_arrive = false;
  transit::Epoch last_arrive_actual = 0;

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
  void registerStop(const std::string& stop_key, const std::string& route, const std::string& dir);

  // Consumes one StopSnapshot for one configured stop and appends any LogEvents it produces to
  // `out`. See DESIGN.md §9.1 for the event rules; the exact tie-breaking rules this
  // implementation uses are documented on the corresponding private helpers in tracker.cpp.
  //
  //   snap                     current arrivals for this configured stop (snap.key identifies it)
  //   now                      current time (unix seconds, UTC)
  //   route_has_live_vehicles  true if this stop's route currently has >=1 realtime-tracked
  //                            vehicle anywhere (used for the noshow-vs-outage rule)
  //   poll_ok                  false if the most recent network poll for this stop's data failed
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
  static TrackedTrip& allocTrip(StopState& st);
  static void freeTrip(TrackedTrip& t);

  void handlePollOutage(StopState& st, transit::Epoch now, bool poll_ok, std::vector<LogEvent>& out);
  void processLiveArrival(StopState& st, const transit::Arrival& a, transit::Epoch now,
                           std::vector<LogEvent>& out);
  void processScheduledArrival(StopState& st, const transit::Arrival& a, transit::Epoch now,
                                bool route_has_live_vehicles, std::vector<LogEvent>& out);
  void reapVanishedAndExpired(StopState& st, transit::Epoch now, std::vector<LogEvent>& out);

  LogEvent baseEvent(const StopState& st, transit::Epoch now, EventType type) const;
};

}  // namespace transit_stats
