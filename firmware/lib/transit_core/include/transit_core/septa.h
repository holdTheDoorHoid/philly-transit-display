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
//
// `dropped` counts items the parser refused to retain because it hit its own size cap (see
// kMaxSchedEntries etc. below). It is not an error - the kept items are valid - but a caller that
// wants to tell the user "showing the first N of many" can read it. `ok` stays true.
template <typename T>
struct ParseResult {
  bool ok = true;
  std::string error;
  std::vector<T> items;
  uint32_t dropped = 0;
};

// --- Retention caps (DESIGN.md 12.1: ~75-80 KB of heap at runtime) ---------------------------
// Every one of these parsers turns an agency-controlled response body into a std::vector. The
// bodies are 1-4 KB in practice, but "in practice" is not a bound: nothing in the protocol stops
// SEPTA (or anything between us and SEPTA) from returning a response with thousands of entries,
// and an unbounded push_back loop would then exhaust the heap and take the device down. Each
// parser reserves its cap once and then keeps only that many items, choosing WHICH to keep by
// what a transit display actually needs (the soonest arrivals - a far-future row is worthless
// next to the next bus). Real payloads are far under these numbers, so nothing is dropped in
// normal operation; see ParseResult::dropped when it is.
// One CYCLE's tracked vehicles that a configured stop can actually use - not one route's whole
// fleet. 32 until 0.3.2-rc3, when the parse gained a filter (TvFilter below) that keeps only the
// vehicles the merge will look up: a rush-hour Route 17 carries 20-30 vehicles, but the owner's
// two stops retain at most 8 GTFS-RT updates per (stop, route) pair, so at most 16 of those
// vehicles are ever read. 16 * 176 B = 2,816 B resident instead of 5,632 B. Vehicles past the cap
// that DID match the filter are counted in ParseResult::dropped and reported as `tv_dropped` on
// GET /api/debug/ui, so a config this number is too small for says so rather than going quiet.
constexpr size_t kMaxTvVehicles = 16;     // relevant tracked vehicles per cycle (real: 2-8)
constexpr size_t kMaxSchedEntries = 24;   // BusSchedules entries per stop, nearest kept (real: 4-12)
constexpr size_t kMaxRailArrivals = 24;   // Arrivals trains per station, nearest kept (real: 10)
constexpr size_t kMaxAlerts = 16;         // alert objects per route (real: 0-2)
// Longest identifier/label string retained from any SEPTA response; longer values are truncated.
// The real ids are 3-8 characters and the longest headsign seen is ~20 ("Lansdale/Doylestown").
constexpr size_t kMaxIdChars = 48;

// Which of a TransitView response's vehicles are worth building at all (0.3.2-rc3).
//
// WHAT merge.cpp ACTUALLY USES A TvVehicle FOR, which is what this filter is derived from:
// exactly one thing. findTvByTrip() finds the vehicle whose `trip` equals a GTFS-RT
// StopTimeUpdate's trip_id, and the caller then reads five fields off it - late, timestamp,
// vehicle_id, destination, seats. Both call sites are inside mergeStop()'s loop over the retained
// realtime updates, so there is no other path into the vector. A vehicle whose trip id is not in
// those updates is never read by anything, and building it costs nine std::strings that are freed
// unexamined at the end of the cycle.
//
// NOT BY DIRECTION, and that is worth saying because it is the filter one reaches for first and
// it is wrong here: TvVehicle::direction is a compass word ("Southbound") while
// StopConfig::direction is a GTFS direction_id ("0"/"1"). Two different id spaces, with no
// mapping anywhere in this project, so comparing them would be a guess dressed as a filter. Nor
// by route: TransitView is fetched per route already, so every vehicle in a response is on a
// configured route by construction. The trip id is the join key the merge uses and it is the only
// honest filter available.
//
// A plain function pointer plus a context pointer rather than a std::function, so installing a
// filter allocates nothing at all, and the predicate is handed the trip id before the TvVehicle
// exists - a rejected vehicle costs one scan of the retained updates and not one allocation.
// A default-constructed TvFilter keeps everything, which is what every caller that brings its own
// vector (host tests, refreshRouteLiveness(), any other consumer of this library) gets.
class TvFilter {
 public:
  using Predicate = bool (*)(const std::string& trip, const void* ctx);

  TvFilter() = default;
  TvFilter(Predicate pred, const void* ctx) : pred_(pred), ctx_(ctx) {}

  bool active() const { return pred_ != nullptr; }
  bool keep(const std::string& trip) const { return pred_ == nullptr || pred_(trip, ctx_); }

 private:
  Predicate pred_ = nullptr;
  const void* ctx_ = nullptr;
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

// The same parse, APPENDING to a result the caller already owns: whatever `out->items` holds is
// left in place and this route's vehicles are added after it, with `out`'s ok/error/dropped reset.
// kMaxTvVehicles applies to the TOTAL, so a multi-route poll cannot walk past the cap either.
//
// The point is the items vector's CAPACITY: a caller that hands the same vector back every cycle
// pays for the block once instead of once per route per cycle. sizeof(TvVehicle) is 176 B on the
// ESP32, so a 32-slot list is 5,632 B - and growing it from reserve(8) by doubling asked for
// 2,816 B and then 5,632 B, both contiguous, both while the entity buffer and the retention block
// were live (DESIGN.md SS5, septa_source.h PollBuffers).
// parseTransitView() above is this with a fresh result.
//
// `filter` decides which vehicles are BUILT (see TvFilter). It is applied before the cap, so
// ParseResult::dropped counts only vehicles that were relevant and did not fit - a filtered-out
// vehicle is not "dropped", it was never wanted. The default keeps everything, so every existing
// caller behaves exactly as it did.
void parseTransitViewAppend(ParseResult<TvVehicle>* out, const uint8_t* data, size_t len,
                             const TvFilter& filter = TvFilter());

// Parses a BusSchedules response: normally `{"<route>": [...], ...}` (usually one key), or
// `{"error": "..."}` on SEPTA's well-documented transient-failure responses (DESIGN.md 4.4;
// NOTES.md has a live-captured example). Detects the latter and returns ok=false with SEPTA's
// message. Note: SchedEntry::route reflects whatever key SEPTA used, which for some modes
// (verified: subway) is a different id scheme than the route id you queried other endpoints
// with - see NOTES.md and merge.h's mergeStop(), which deliberately does not require the two to
// match.
ParseResult<SchedEntry> parseBusSchedules(const uint8_t* data, size_t len);

// The same parse, writing into a result the caller already owns: `out` is reset (items cleared,
// ok/error/dropped restored) with its items vector's CAPACITY kept, so a caller that hands the
// same result back each time pays the kMaxSchedEntries reserve() once rather than per fetch.
// parseBusSchedules() above is this with a fresh result.
void parseBusSchedulesInto(ParseResult<SchedEntry>* out, const uint8_t* data, size_t len);

// The earliest still-upcoming `DateCalender` in a RAW BusSchedules body, as epoch seconds, or 0
// if the body carries none that parse. "Still upcoming" is `>= now - 60`, the same one-minute
// grace fetchPlausibleSchedule() uses, so the two agree about which trip is first.
//
// This is a byte scan over the response exactly as it arrived: no JsonDocument, no copy of the
// body, and no std::string per entry (the value is unescaped into a stack buffer - SEPTA sends
// "09\/15\/26 12:32 am"). It exists because the firmware has to judge SEPTA's service day at the
// TRANSPORT layer, before transit_core parses anything: only that layer sees which backend
// answered, and only a fresh connection without the sticky cookie can land on a different one
// (NOTES.md 9). The app-side version of this made `std::string text(body.begin(), body.end())` -
// a second full copy of a body that is capped at 4 KB, asked for as one contiguous block while
// the GTFS-RT entity and retention buffers were still live (audit_runtime SS3 #12).
Epoch firstUpcomingScheduleTime(const uint8_t* data, size_t len, Epoch now);

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

// Same table, but accepts EITHER the short code ("PAO") or the display name ("Paoli/Thorndale"),
// case-insensitively in both cases. Returns nullptr if neither matches.
//
// Why both: StopConfig::route for Mode::Rail is documented as the line CODE, but configs written
// before that was settled carry the display name that Arrivals itself reports (the string the
// stop-picker UI showed). Accepting both here means such a config keeps working instead of
// silently filtering every train away (an unrecognized code matches nothing by design - see
// mergeRail()), and gives the config layer one place to normalise a legacy value from.
const RailLine* findRailLineByName(const std::string& code_or_display_name);

}  // namespace transit
