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
//   - `sched` is filtered only by direction (SchedEntry::direction == cfg.direction, when
//     cfg.direction is non-empty) - deliberately NOT by SchedEntry::route == cfg.route, because
//     BusSchedules is already scoped to cfg.stop_id and for some modes (verified: subway) it
//     reports trips under a different route-id scheme than the one used elsewhere (see
//     NOTES.md). Each surviving live arrival is matched to the nearest unconsumed SchedEntry
//     within +/-600s of (predicted - late_min*60) (or of predicted itself, when lateness isn't
//     known); any SchedEntry left unmatched with scheduled > now-60 becomes its own
//     Status::Scheduled row (this is how a Subway-mode stop, which has no realtime source at
//     all - pass empty `rt`/`tv` for it - ends up schedule-only, per DESIGN.md 4.6).
//
// Output arrivals are sorted by Arrival::effective() ascending; any arrival (live or scheduled)
// with effective() < now-60 is dropped. StopSnapshot::ok is always true and ::error always empty
// - mergeStop has no notion of fetch failure; a caller that couldn't fetch a source for this
// stop at all should set those fields itself (or skip merging and report the failure directly).
StopSnapshot mergeStop(const StopConfig& cfg, const std::vector<StopTimeUpdate>& rt,
                        const std::vector<TvVehicle>& tv, const std::vector<SchedEntry>& sched,
                        Epoch now);

// Builds one Regional Rail stop's snapshot from Arrivals results, per DESIGN.md 4.6.
// Filters `rail` to entries matching cfg.direction ("N"/"S"; "" means both) and, if cfg.route is
// non-empty, to entries whose RailArrival::line corresponds to that line code (see NOTES.md for
// the line-code-to-display-name table; an unrecognized code matches nothing rather than
// silently passing everything through).
//
// RailArrival::status is mapped to Arrival::late_min/late_known as: "On Time" -> 0/known; a
// signed "<N> min" -> N/known (SEPTA's own convention, mirroring TransitView's `late`: positive
// is late, negative is early); anything else (e.g. "Delayed", "Suspended") -> 0/not known, but
// the arrival is still emitted using its predicted/scheduled time. Arrival::predicted is
// RailArrival::depart (SEPTA's live estimate); Arrival::scheduled is RailArrival::sched;
// Arrival::status is always Status::Live (Arrivals has no schedule-only/skipped concept).
//
// Same sort/staleness rules as mergeStop; StopSnapshot::ok/::error default the same way.
StopSnapshot mergeRail(const StopConfig& cfg, const std::vector<RailArrival>& rail, Epoch now);

}  // namespace transit
