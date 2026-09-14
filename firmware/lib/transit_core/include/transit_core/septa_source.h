// SEPTA implementation of the transit_core TransitSource interface (source.h), plus the
// concrete URL builders and per-poll-cycle orchestration used by the firmware net_poller.
// Arduino-independent (host + ESP32); all facts encoded here (prefixes, param names, which
// modes need which quirk workaround) were verified live on 2026-09-13 - see NOTES.md.
#pragma once
#include <string>
#include <vector>

#include "transit_core/model.h"
#include "transit_core/septa.h"
#include "transit_core/source.h"

namespace transit {

// --- URL builders --------------------------------------------------------------------------
// Every builder returns a complete https://www3.septa.org/... URL. SEPTA identifiers (route
// ids, stop ids, station names) only ever need spaces percent-encoded in practice; that's all
// these do.

std::string septaTripUpdatesUrl();
std::string septaTransitViewUrl(const std::string& route);
std::string septaBusSchedulesUrl(const std::string& stop_id);
std::string septaArrivalsUrl(const std::string& station, const std::string& direction = "");

// Builds the Alerts `routes=` value for one configured route/line (NOTES.md 7b has the full
// live-verified prefix table this follows):
//   Mode::Bus     -> "bus_route_<route>"      e.g. "bus_route_17", "bus_route_BLVDDIR"
//   Mode::Trolley -> "trolley_route_<route>"  e.g. "trolley_route_10"
//   Mode::Subway  -> "rr_route_<lowercase route>"  e.g. "rr_route_bsl" for route "BSL" - verified
//                    live; the subway alert feed uses the Regional-Rail prefix, NOT bus_route_,
//                    unlike every other subway-facing endpoint (NOTES.md 7a/7b).
//   Mode::Rail    -> "rr_route_<alert_suffix>" looked up via findRailLine(route) - the suffix is
//                    NOT always lowercase(route) (e.g. "TRE" -> "rr_route_trent", not "rr_route_tre").
// Returns "" if `route` is empty, or (Mode::Rail only) if the code isn't in kRailLines - there
// is no safe generic fallback for Regional Rail's irregular suffixes.
std::string alertRouteIdFor(Mode mode, const std::string& route);

// septaAlertsUrl returns "" (no URL) under the same conditions alertRouteIdFor() returns "".
std::string septaAlertsUrl(Mode mode, const std::string& route);

// SEPTA implementation of TransitSource (DESIGN.md 11).
//
// Memory: SeptaSource itself holds no members (stateless; one vtable pointer, 4 bytes on
// ESP32). Each fetch method owns exactly one transient std::vector<uint8_t> body buffer, capped
// at kJsonBodyCap (16 KB, generous headroom over the ~1-4 KB real payloads - DESIGN.md 4.3-4.6)
// and freed when the method returns; fetchRealtime never buffers a body at all (see
// gtfsrt_stream.h). pollBusStops()/pollRailStops() additionally hold the merged StopTimeUpdate/
// TvVehicle/SchedEntry/RailArrival vectors for one poll cycle - bounded by the feed's real size
// (a few hundred StopTimeUpdate structs at most) and freed on return.
class SeptaSource : public TransitSource {
 public:
  int fetchRealtime(GtfsRtStream& stream, HttpGet http) override;
  int fetchSchedule(const std::string& stop_id, std::vector<SchedEntry>* out,
                     HttpGet http) override;
  int fetchAlerts(Mode mode, const std::string& route, std::vector<transit::Alert>* out,
                   HttpGet http) override;

  // Not part of the portable TransitSource interface (no other agency modeled in this project
  // has an equivalent endpoint): TransitView vehicle positions for one route. Same out-param
  // contract as fetchSchedule (unmodified on failure).
  int fetchTransitView(const std::string& route, std::vector<TvVehicle>* out, HttpGet http);

  // Regional Rail Arrivals for one station. Same out-param contract as fetchSchedule.
  int fetchRailArrivals(const std::string& station, std::vector<RailArrival>* out, HttpGet http);
};

// Orchestrates one full poll cycle for every Mode::Bus/Mode::Trolley/Mode::Subway entry in
// `configs` (DESIGN.md 4.7, 11):
//   1. One GTFS-RT TripUpdates fetch, streamed through a single GtfsRtStream filtered to the
//      union of all Bus/Trolley configs' routes and stop_ids (subway carries no GTFS-RT trips -
//      verified NOTES.md 7a - so subway configs are not added to this filter).
//   2. One TransitView fetch per distinct Bus/Trolley route (same reason subway is excluded).
//   3. One BusSchedules fetch per distinct stop_id across Bus/Trolley/Subway configs that isn't
//      already fresh in `cache` (BusSchedules works for subway stop_ids too - NOTES.md 7a).
//   4. mergeStop() per StopConfig, using empty rt/tv vectors for Subway configs - which is what
//      makes a subway stop come out schedule-only with no special-casing (see merge.h).
// Mode::Rail configs are ignored here - see pollRailStops(). Never buffers the ~150 KB
// TripUpdates body whole; opens exactly one connection per distinct URL needed, per DESIGN.md 5.
Snapshot pollBusStops(const std::vector<StopConfig>& configs, Epoch now, HttpGet http,
                      ScheduleCache& cache);

// Orchestrates one poll cycle for every Mode::Rail entry in `configs`: one Arrivals fetch per
// distinct station, then mergeRail() per StopConfig sharing that station.
Snapshot pollRailStops(const std::vector<StopConfig>& configs, Epoch now, HttpGet http);

}  // namespace transit
