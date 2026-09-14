// Parsers from SEPTA v1 JSON API response bytes into plain structs. Arduino-independent (host +
// ESP32); uses ArduinoJson 7 (the project's only JSON dependency) and nothing else. See
// DESIGN.md 4.3-4.6 and docs/research/septa-data-sources.md for the endpoints and real payload
// shapes these parsers were written against (every shape below was verified live on
// 2026-09-13, see firmware/lib/transit_core/NOTES.md).
//
// None of these functions hold state or allocate beyond the single ArduinoJson document used to
// parse one response and the output vector it fills - callers own the input bytes and the
// output vector's lifetime. Typical response bodies are 1-4 KB (TransitView, BusSchedules,
// Alerts, Arrivals are all per-route/per-stop/per-station, never the multi-hundred-KB
// system-wide variants - DESIGN.md 4.3-4.5 forbids ever fetching those). ArduinoJson7's
// JsonDocument grows to fit the input; for these payload sizes it typically uses on the order of
// 2-4x the input bytes internally, which stays well under 20 KB even for the largest fixture
// used here (arrivals_30th.json, ~3.5 KB). Never call these on an unbounded/streamed body -
// only gtfsrt_stream.h is designed for that.
//
// SEPTA's v1 API is inconsistent about whether numeric-looking fields are JSON numbers or
// JSON strings (observed: TransitView's `lat`/`lng` and BusSchedules' nothing are quoted
// strings, while `late`/`next_stop_sequence`/`timestamp` are bare numbers - see NOTES.md).
// These parsers tolerate either representation for every field (see the private jsonToString/
// jsonToInt/jsonToDouble helpers in septa.cpp) rather than assuming one or the other.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "transit_core/model.h"

namespace transit {

// Generic "did it work" wrapper so these parsers can report a clear error without exceptions
// (this project builds with -fno-exceptions). On failure, `items` is empty and `error` holds a
// short human-readable reason (either SEPTA's own `{"error": "..."}` message, verbatim, or a
// description of why the shape didn't match what was expected).
template <typename T>
struct ParseResult {
  bool ok = true;
  std::string error;
  std::vector<T> items;
};

// One vehicle from TransitView/index.php?route=<route> (DESIGN.md 4.3).
struct TvVehicle {
  std::string trip;          // == GTFS-RT TripDescriptor.trip_id (DESIGN.md 4.2 join key)
  std::string vehicle_id;    // TransitView "VehicleID"; may be the literal string "None"
  int late = 0;              // TransitView "late", minutes; SEPTA also uses it as a sentinel:
                              // 998/999 observed for vehicles with no current GPS fix - see
                              // NOTES.md. Callers should treat |late| >= 900 as "not known".
  std::string destination;
  std::string direction;     // compass form, e.g. "Southbound", or "N/A"
  std::string next_stop_id;
  uint32_t next_stop_sequence = 0;
  std::string seats;         // "estimated_seat_availability", e.g. "FEW_SEATS_AVAILABLE"
  int64_t timestamp = 0;     // already epoch seconds, no local-time parsing needed
  double lat = 0.0;
  double lng = 0.0;
};

// One scheduled trip from BusSchedules/index.php?stop_id=<id> (DESIGN.md 4.4). `route` comes
// from the response's top-level key (e.g. the "17" in `{"17": [...]}`), not a field inside the
// entry - BusSchedules has no `route` query parameter, only `stop_id`, so a stop's own response
// can in principle carry more than one route key; parseBusSchedules flattens all of them.
struct SchedEntry {
  std::string route;
  std::string trip_id;        // static-GTFS trip id; does NOT match GTFS-RT/TransitView trip ids
  Epoch scheduled = 0;        // parsed from "DateCalender" via timeparse::parseBusScheduleTime
  std::string direction;      // "0" or "1"
  std::string direction_desc; // e.g. "20th-Johnston"
  std::string stop_name;
};

// One train from Arrivals/index.php?station=<name> (DESIGN.md 4.6).
struct RailArrival {
  std::string direction;    // "N" or "S" (mapped from the response's "Northbound"/"Southbound"
                             // grouping keys - DESIGN.md 4.6: these are legacy division names,
                             // not compass directions)
  std::string train_id;
  std::string line;         // human display name, e.g. "Fox Chase" (see NOTES.md for the
                             // mapping from these names to Alerts' rr_route_* short codes)
  std::string destination;
  std::string origin;
  std::string status;       // e.g. "On Time", "3 min" - see mergeRail() for how this becomes
                             // Arrival::late_min
  Epoch sched = 0;          // parsed "sched_time"
  Epoch depart = 0;         // parsed "depart_time"; SEPTA's live estimate, already reflects delay
  std::string track;
  std::string next_station;
};

// Parses a TransitView response. Accepts both the normal `{"bus": [...]}` shape and the bare
// `[]` SEPTA returns for a route with no currently-tracked vehicles or an unrecognized route id
// (verified: TransitView never returns a `{"error": ...}` body - an empty result is the only
// failure signal it gives). `data`/`len` are the raw response bytes (NOT null-terminated
// required).
ParseResult<TvVehicle> parseTransitView(const uint8_t* data, size_t len);

// Parses a BusSchedules response: normally `{"<route>": [...], ...}` (usually one key), or
// `{"error": "..."}` on SEPTA's well-documented transient-failure responses (DESIGN.md 4.4;
// NOTES.md has a live-captured example). Detects the latter and returns ok=false with SEPTA's
// message. Note: SchedEntry::route reflects whatever key SEPTA used, which for some modes
// (verified: subway) is a different id scheme than the route id you queried other endpoints
// with - see NOTES.md and merge.h's mergeStop(), which deliberately does not require the two to
// match.
ParseResult<SchedEntry> parseBusSchedules(const uint8_t* data, size_t len);

// Parses an Alerts response: a JSON array of per-route alert objects (possibly empty - SEPTA
// also silently returns `[]` for a syntactically-valid-but-wrong `routes=` value, which is
// indistinguishable from "no current alerts"; see NOTES.md). `Alert::text` prefers the `advisory`
// field (HTML), falling back to `alert` then `description`; HTML tags and common entities are
// stripped, whitespace is collapsed, and the result is trimmed to ~240 characters. Each
// `detour[]` entry becomes one plain-text string in `Alert::detours` ("<reason>: <message>", or
// just `<message>` if reason is blank).
ParseResult<transit::Alert> parseAlerts(const uint8_t* data, size_t len);

// Parses an Arrivals response: `{"<dynamic title>": [ {"Northbound": [...]}, {"Southbound":
// [...]} ]}`, or `{"error": "..."}` for an unrecognized station name (DESIGN.md 4.6; NOTES.md
// documents which GTFS-static station names do and do not work here). The outer key's exact
// text varies per request/time and is ignored - only its (single) value is used.
ParseResult<RailArrival> parseRailArrivals(const uint8_t* data, size_t len);

// Regional Rail line reference data (NOTES.md 7b: display names come from live Arrivals/
// TrainView responses, alert_suffix from a live, unfiltered Alerts/index.php listing). `code` is
// the short id used as StopConfig::route for Mode::Rail (matches GTFS static routes.txt
// route_id/route_short_name, e.g. "FOX", "MED", "WAR").
struct RailLine {
  const char* code;          // e.g. "FOX"
  const char* display_name;  // e.g. "Fox Chase" - matches RailArrival::line, used by mergeRail()
  const char* alert_suffix;  // e.g. "fxc" - forms Alerts' "rr_route_fxc", used by septa_source
};

// All 13 SEPTA Regional Rail lines this project could verify end-to-end (NOTES.md 7b). Excludes
// "GLN" / rr_route_gc, seen in the Alerts route listing but never as an Arrivals/TrainView line
// display name - see NOTES.md for what was and wasn't confirmed.
extern const RailLine kRailLines[];
extern const size_t kRailLineCount;

// Looks up `code` (case-sensitive, e.g. "FOX") in kRailLines; returns nullptr if not found.
const RailLine* findRailLine(const std::string& code);

}  // namespace transit
