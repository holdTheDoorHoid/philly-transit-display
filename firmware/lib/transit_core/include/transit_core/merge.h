// Combines the outputs of gtfsrt_stream.h and septa.h into the shared transit::StopSnapshot
// model, per DESIGN.md 4 and 7-8. Arduino-independent (host + ESP32). Pure functions: no state,
// no allocation beyond the output vector and a couple of small (O(sched.size())) scratch
// vectors local to mergeStop's schedule-matching pass, freed on return.
#pragma once
#include <vector>

#include "transit_core/gtfsrt_stream.h"
#include "transit_core/model.h"
#include "transit_core/septa.h"

namespace transit {

// What the fetch layer managed to get for one stop this cycle. mergeStop()/mergeRail() cannot
// tell "SEPTA returned no trips" from "the request failed" on their own - both arrive as an
// empty vector - so the caller states it here, and the merge turns it into StopSnapshot::ok /
// ::health / ::error. Defaults say "everything worked", which is what a merge-only unit test or
// a caller with nothing to report wants.
//
// Which fields a stop actually depends on follows its Mode (DESIGN.md 4.2-4.6):
//   Bus/Trolley  live_ok (GTFS-RT TripUpdates) + vehicles_ok (TransitView) + schedule_ok
//   Subway       schedule_ok only - there is no realtime source at all (NOTES.md 7a)
//   Rail         live_ok only, meaning the Arrivals API (mergeRail ignores the other two)
struct SourceStatus {
  bool live_ok = true;      // realtime feed fetched and its body was complete and parseable
  bool vehicles_ok = true;  // TransitView fetched for this stop's route
  bool schedule_ok = true;  // BusSchedules fetched (or served fresh from cache) for this stop_id
};

// How far out of date a realtime feed's own timestamp may be before its predictions stop being
// treated as live (DESIGN.md 4.7 polls every 30 s, so five minutes is ten missed cycles), and how
// far AHEAD of `now` a feed timestamp may be before the feed is disbelieved rather than the
// clock. The forward bound matters because an old feed can legitimately contain future
// predictions: without it, a replayed body reads as a perfectly fresh set of arrivals.
constexpr Epoch kFeedStaleAfterS = 5 * 60;
constexpr Epoch kFeedFutureToleranceS = 5 * 60;

// True if a BusSchedules entry belongs to the route this StopConfig is displaying.
//
// This is NOT plain string equality, and it is not "anything goes" either - both are wrong in
// ways that put another route's departures under this route's heading:
//   - Bus/Trolley: exact match (case-insensitive). BusSchedules is scoped by stop_id only, and a
//     shared stop returns every route that serves it in one response, so route 17 and route 2 at
//     the same corner come back together. Matching on direction alone made route 17's panel show
//     route 2's schedule, and let a live route-17 arrival "match" (and consume) a route-2
//     scheduled time, corrupting its lateness.
//   - Subway: BusSchedules reports subway trips under the GTFS-static route ids, not the id the
//     rest of the system uses - a "BSL" stop's schedule comes back keyed "B1" (verified live,
//     NOTES.md 7a). So Mode::Subway matches through kSubwaySchedAliases below, which accepts the
//     configured id itself plus its known GTFS ids.
//   - Rail: not applicable (mergeRail has its own line filter); treated like bus for safety.
// An empty cfg.route means "no route filter", as everywhere else in this file.
bool schedRouteMatches(const StopConfig& cfg, const SchedEntry& entry);

// Builds one bus/trolley/subway stop's snapshot by joining the GTFS-RT stream, TransitView, and
// BusSchedules results for that stop, per DESIGN.md 4.2/4.4 and 7.
//
// All three input vectors may be broader than this one stop (e.g. `rt`/`tv` for every configured
// stop on a shared route, or `sched` for a stop shared by multiple StopConfigs): mergeStop
// filters internally rather than requiring the caller to pre-scope them.
//   - `rt` is filtered to entries whose stop_id == cfg.stop_id, route_id == cfg.route, and
//     (if cfg.direction is non-empty and the entry's direction_id is known) direction_id ==
//     atoi(cfg.direction).
//   - `tv` is joined to the surviving `rt` entries by trip id only (DESIGN.md 4.2: TransitView's
//     `trip` field equals GTFS-RT's trip_id) - never by direction, since TvVehicle carries no
//     stop_id and its compass-form Direction string has no fixed mapping to GTFS direction_id.
//     A `tv` match supplies late_min/late_known/seats, and is preferred over `rt`'s own
//     vehicle_id and (for destination) cfg.headsign when present.
//   - `sched` is filtered by direction (SchedEntry::direction == cfg.direction, when
//     cfg.direction is non-empty) AND by schedRouteMatches() above, in BOTH the live-matching
//     pass and the schedule-only fallback. Each surviving live arrival is matched to the nearest
//     unconsumed SchedEntry within +/-600s of (predicted - late_min*60) (or of predicted itself,
//     when lateness isn't known); the matched entry's trip_id is kept on Arrival::sched_trip so
//     the stats tracker can reconcile the scheduled and live records for one static trip. Any
//     SchedEntry left unmatched with scheduled > now-60 becomes its own Status::Scheduled row
//     (this is how a Subway-mode stop, which has no realtime source at all - pass empty `rt`/`tv`
//     for it - ends up schedule-only, per DESIGN.md 4.6).
//
// Negative service information is represented, not dropped (DESIGN.md 4.2):
//   - A stop-level SKIPPED update becomes a Status::Skipped row and CONSUMES its matched schedule
//     entry, so the detour is shown instead of the fallback quietly re-advertising the same trip
//     as a normal scheduled arrival. A SKIPPED update with no time of its own is not discarded
//     for lack of one: it takes the soonest still-unclaimed matching schedule entry and is kept
//     while that scheduled time is > now-60.
//   - A trip-level CANCELED/DELETED trip produces no row at all AND consumes the schedule entry
//     its predicted time matches, so a cancelled trip is never shown as scheduled. (With no
//     predicted time there is nothing to match it to - static and realtime trip ids are different
//     id spaces, DESIGN.md 4.4 - so such a trip can only be suppressed where it carries a time.)
//   - A NO_DATA update is not a prediction and no time is invented for it: it is ignored, leaving
//     the stop's schedule row to speak for that trip.
//
// Feed age (DESIGN.md 4.7): the newest StopTimeUpdate::feed_timestamp among the surviving `rt`
// entries (or the newest plausible TvVehicle::timestamp, when TransitView is all there is)
// becomes StopSnapshot::source_ts - when the AGENCY produced the data, as opposed to `fetched`,
// when we asked for it. If that is more than kFeedStaleAfterS before `now`, or more than
// kFeedFutureToleranceS after it, the feed is treated as stale: the stop becomes Health::Stale
// and every realtime-derived row is demoted to Status::Scheduled (keeping its matched schedule
// time) or dropped if it has no schedule time to fall back on. A feed that published NO
// timestamp is "unknown age": source_ts stays 0 and the rows are left alone - today's behaviour,
// deliberately, because inventing freshness and inventing staleness are both wrong.
//
// Output arrivals are sorted by Arrival::effective() ascending; any arrival with effective() <
// now-60 is dropped (a Skipped row is kept on its scheduled time under the same rule).
// StopSnapshot::ok/::health/::error come from `sources` and the staleness check above - see
// model.h for exactly what the two flags mean and how Snapshot::last_poll_ok is derived.
StopSnapshot mergeStop(const StopConfig& cfg, const std::vector<StopTimeUpdate>& rt,
                        const std::vector<TvVehicle>& tv, const std::vector<SchedEntry>& sched,
                        Epoch now, const SourceStatus& sources = SourceStatus());

// Builds one Regional Rail stop's snapshot from Arrivals results, per DESIGN.md 4.6.
// Filters `rail` to entries matching cfg.direction ("N"/"S"; "" means both) and, if cfg.route is
// non-empty, to entries whose RailArrival::line corresponds to that line. The line is resolved
// with findRailLineByName(), so cfg.route may be either the code ("PAO") or the display name
// ("Paoli/Thorndale") a pre-normalisation config carries; an unrecognized value still matches
// nothing rather than silently passing everything through.
//
// RailArrival::status is mapped to Arrival::late_min/late_known as: "On Time" -> 0/known; a
// signed "<N> min" -> N/known (SEPTA's own convention, mirroring TransitView's `late`: positive
// is late, negative is early); anything else (e.g. "Delayed", "Suspended") -> 0/not known, but
// the arrival is still emitted using its predicted/scheduled time. Arrival::predicted is
// RailArrival::depart (SEPTA's live estimate); Arrival::scheduled is RailArrival::sched;
// Arrival::status is always Status::Live (Arrivals has no schedule-only/skipped concept).
//
// Same sort/staleness rules as mergeStop. Arrivals publishes no "generated at" timestamp, so
// StopSnapshot::source_ts is `now` (the response is generated per request) and there is no feed-
// age check to make. `sources.live_ok` is the Arrivals fetch; the other SourceStatus fields are
// not consulted.
StopSnapshot mergeRail(const StopConfig& cfg, const std::vector<RailArrival>& rail, Epoch now,
                        const SourceStatus& sources = SourceStatus());

}  // namespace transit
