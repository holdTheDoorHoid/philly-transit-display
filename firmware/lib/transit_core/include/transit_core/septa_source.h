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

// What one SEPTA fetch actually achieved: the transport's own report plus the verdict on the
// body. Returning only an HTTP status (what these methods used to do) threw away the parser's
// error message and made "SEPTA answered 200 with a body we could not understand"
// indistinguishable from success - which is how a stop with no usable data ended up displayed as
// a stop with nothing due.
//
// `ok` is judged from the BODY, never from the status code alone: SEPTA has been observed
// serving perfectly valid BusSchedules JSON under HTTP 501 and a genuine {"error": ...} under
// HTTP 400 (NOTES.md 1), so validating the payload is the only reliable test and is deliberately
// preserved here.
struct FetchOutcome {
  FetchResult transport;
  bool ok = false;      // usable data was parsed out of the response
  std::string error;    // short, human-readable, empty when ok
};

// --- The poll cycle's reusable working set (DESIGN.md 5, "the poll working set") -------------
//
// Everything one poll cycle needs a sizeable buffer for, in ONE long-lived object the caller
// allocates once - on this device, before Wi-Fi, out of a heap that is still a single run - and
// hands to every fetch of every cycle thereafter.
//
// WHY. A cycle used to construct these per call: a GtfsRtStream (4,096 B entity buffer +
// ~4,600 B retention block), a std::vector<uint8_t> body grown from nothing by doubling for each
// TransitView / BusSchedules / Alerts / Arrivals response, and a 24 x 128 B SchedEntry block per
// schedule parse. Every one of them is a CONTIGUOUS request, all of them land inside a few
// hundred milliseconds of each other, and the free heap on the target is not the constraint - the
// largest free BLOCK is. Measured on the owner's board, that block rests at 25-28 KB and decays
// with uptime and request rate; at 11.7 KB (seen 2026-09-16, free heap still 36 KB) the cycle
// threw std::bad_alloc, and every cycle after it did the same until the heap-wedge self-heal
// rebooted the device. Reserved once, up front, none of these is ever asked for again.
//
// LIFETIME AND REENTRANCY. One cycle at a time, on one task: pollBusStops(), pollRailStops() and
// the firmware's alerts/liveness helpers run one after another on the poller task and never nest.
// `body` holds one response at a time; `sched` is the parse target for one stop's schedule, which
// is copied out size-exact before the next stop is fetched. Passing a null PollBuffers* (the
// default everywhere) restores exactly the old per-call behaviour, which is what the host tests
// and any other consumer of this library get.
struct PollBuffers {
  // The TripUpdates decoder. reset() per cycle keeps the two blocks above.
  GtfsRtStream rt{0};
  // One buffered JSON response body (TransitView, BusSchedules, Alerts, Arrivals). Capped by
  // kJsonBodyCap inside fetchBuffered(), as before.
  std::vector<uint8_t> body;
  // The BusSchedules parse target, kept at kMaxSchedEntries capacity across cycles.
  ParseResult<SchedEntry> sched;

  // What body is reserved to, and trimmed back to. 4 KB covers every response this project
  // fetches (real payloads are 0.8-4 KB - DESIGN.md 4.3-4.6) and is the hard cap the firmware's
  // own BusSchedules buffering applies; a larger one is still accepted, it just is not kept.
  static constexpr size_t kBodyReserve = 4096;

  // Call once, before the first cycle, while the heap is unfragmented.
  void reserveAll(size_t entity_bytes = 4096,
                   size_t retained = GtfsRtStream::kDefaultMaxRetainedUpdates,
                   size_t sched_entries = kMaxSchedEntries);

  // Call at the top of each cycle. Clears without releasing, and releases only an oversized body
  // buffer - a one-off large response must not become resident for the life of the device. Doing
  // that here rather than at the end of a cycle means the replacement 4 KB block is asked for at
  // the point in the cycle where the largest free block is at its healthiest.
  void beginCycle();
};

// SEPTA implementation of TransitSource (DESIGN.md 11).
//
// Every fetch has two forms:
//   fetchX(..., HttpGet)     legacy: returns the HTTP status, keeps working for callers that
//                            have no completeness information to give (the TransitSource
//                            interface, the firmware's alerts/TransitView helpers).
//   fetchXEx(..., HttpGetEx) returns a FetchOutcome, and is what pollBusStops()/pollRailStops()
//                            use so a failure can reach the per-stop health fields.
// The legacy forms are implemented on top of the Ex ones through adaptHttpGet(), so there is one
// implementation of each fetch, not two.
//
// Memory: SeptaSource holds one pointer (plus its vtable pointer). By default each fetch method
// owns exactly one transient std::vector<uint8_t> body buffer, capped at kJsonBodyCap (16 KB,
// generous headroom over the ~1-4 KB real payloads - DESIGN.md 4.3-4.6) and freed when the method
// returns; fetchRealtime never buffers a body at all (see gtfsrt_stream.h).
// pollBusStops()/pollRailStops() additionally hold the merged StopTimeUpdate/TvVehicle/
// SchedEntry/RailArrival vectors for one poll cycle - each bounded by its own retention cap
// (GtfsRtStream::retainUpdates, septa.h's kMax* constants) and freed on return.
//
// Constructed with a PollBuffers*, the body buffer and the schedule parse target come from there
// instead, so a cycle asks the allocator for neither (see PollBuffers above). Nothing else
// changes: the same bodies are buffered under the same cap and the same out-parameter contract
// holds - a fetch that fails leaves `*out` untouched.
class SeptaSource : public TransitSource {
 public:
  SeptaSource() = default;
  explicit SeptaSource(PollBuffers* buffers) : buffers_(buffers) {}

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

  // Completeness-aware forms. `ok` means "usable data": for the realtime feed, a body that
  // arrived whole and framed correctly (GtfsRtStream::finish() == FeedStatus::Complete); for the
  // JSON endpoints, a body that parsed and was not SEPTA's {"error": ...} shape. A truncated
  // body is a failure even under HTTP 200.
  FetchOutcome fetchRealtimeEx(GtfsRtStream& stream, HttpGetEx http);
  FetchOutcome fetchScheduleEx(const std::string& stop_id, std::vector<SchedEntry>* out,
                                HttpGetEx http);
  FetchOutcome fetchAlertsEx(Mode mode, const std::string& route,
                              std::vector<transit::Alert>* out, HttpGetEx http);
  FetchOutcome fetchTransitViewEx(const std::string& route, std::vector<TvVehicle>* out,
                                   HttpGetEx http);
  FetchOutcome fetchRailArrivalsEx(const std::string& station, std::vector<RailArrival>* out,
                                    HttpGetEx http);

 private:
  PollBuffers* buffers_ = nullptr;  // null: every fetch owns its own transient buffers
};

// SEPTA's BusSchedules backend is not consistent across requests: some of the servers behind it
// hold a stale schedule with no service for the current day and answer with the first trips of
// the *next* day. Observed 2026-09-14 at 09:15 for stop 21297: two of five identical requests
// returned trips at 12:32 am the following day (under the old "Front-Market" headsign), the other
// three returned 9:30 am today (NOTES.md 9). Shown naively, that is "887 minutes" on the display.
//
// fetchPlausibleSchedule() calls SeptaSource::fetchSchedule up to kScheduleFetchAttempts times
// while the earliest upcoming entry is more than kSchedulePlausibleS away, and keeps the response
// whose first upcoming trip is soonest. It stops early on a TRANSPORT failure (FetchResult::status
// <= 0, "the request could not be made at all"): the retries here are for a backend that answers
// with the wrong service day, and the transport has already spent its own attempts and backoff on
// the URL, so asking again only multiplies a dead network by three.
// Returns true if the kept response looked plausible (first
// upcoming trip within kSchedulePlausibleS); false if every attempt looked wrong (the best one is
// still written to *out so a genuinely sparse overnight schedule is displayed) or nothing usable
// came back at all (*out untouched). Callers cache a false result only briefly (ScheduleCache::
// putSuspect).
//
// `fetched_ok` (when non-null) reports whether ANY attempt produced usable data, which is a
// different question from whether the answer looked plausible: a stop whose schedule endpoint
// failed outright must mark that stop unavailable, while a stop that merely got a wrong-service-
// day answer still has data to show.
constexpr int kScheduleFetchAttempts = 3;
constexpr Epoch kSchedulePlausibleS = 2 * 3600;
bool fetchPlausibleSchedule(SeptaSource& src, const std::string& stop_id, Epoch now, HttpGet http,
                            std::vector<SchedEntry>* out, bool* fetched_ok = nullptr);
bool fetchPlausibleSchedule(SeptaSource& src, const std::string& stop_id, Epoch now,
                            HttpGetEx http, std::vector<SchedEntry>* out,
                            bool* fetched_ok = nullptr);

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
//
// Failure is reported PER STOP, not globally (DESIGN.md 7). Each source's outcome is routed to
// the stops that actually depend on it, via mergeStop()'s SourceStatus: a BusSchedules failure
// for one subway station marks that station Unavailable and leaves every bus stop alone, while a
// truncated TripUpdates body marks the bus stops (which still show their schedule rows, flagged
// Health::ScheduleOnly) and leaves the subway alone. Snapshot::last_poll_ok is then simply "every
// stop's required sources succeeded", and last_error the first stop error seen.
//
// `buffers`, when non-null, is the caller's long-lived working set (PollBuffers above): the
// TripUpdates decoder, the response body buffer and the schedule parse target all come from it
// instead of being constructed for this call. Null (the default) behaves exactly as before.
Snapshot pollBusStops(const std::vector<StopConfig>& configs, Epoch now, HttpGetEx http,
                      ScheduleCache& cache, PollBuffers* buffers = nullptr);
Snapshot pollBusStops(const std::vector<StopConfig>& configs, Epoch now, HttpGet http,
                      ScheduleCache& cache);

// Orchestrates one poll cycle for every Mode::Rail entry in `configs`: one Arrivals fetch per
// distinct station, then mergeRail() per StopConfig sharing that station. One station failing
// marks only the stops configured for that station, not the whole poll.
Snapshot pollRailStops(const std::vector<StopConfig>& configs, Epoch now, HttpGetEx http,
                       PollBuffers* buffers = nullptr);
Snapshot pollRailStops(const std::vector<StopConfig>& configs, Epoch now, HttpGet http);

}  // namespace transit
