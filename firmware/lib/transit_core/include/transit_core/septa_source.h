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

// --- The poll cycle's shared scratch buffer (DESIGN.md 5, "the poll working set") -------------
//
// ONE buffer, reserved once by the caller (on this device, in setup() before Wi-Fi) and lent to
// each stage of a poll cycle IN TURN.
//
// WHY ONE AND NOT SIX. The first attempt at this gave every consumer its own permanent buffer -
// the GTFS-RT entity buffer, the retention block, a response body buffer, the schedule parse
// block, the transport's own BusSchedules buffer and the Indego feature buffer, about 26 KB in
// all. Measured on the owner's board (0.3.1-rc1, 2026-09-17) that worked exactly as designed and
// was still a net loss: the per-cycle CONTIGUOUS demand fell from ~12 KB to ~5 KB, but the resting
// free heap fell from ~35 KB to ~22 KB and the resting largest block from ~24.5 KB to ~14 KB, so
// the margin that actually matters - largest block minus demand - went from ~12 KB to ~9 KB. Worse,
// `min_free8` reached 696 B at 115 s of uptime, with a poll mid-fetch and one concurrent
// /api/state on the other task: a trough to zero is an lwIP assert, not a caught bad_alloc
// (DESIGN.md 12.1). The FLOOR matters as much as the demand, and holding memory permanently is
// how you lower the floor.
//
// The fix is that these buffers are never live at the same time. They are all "one thing at a
// time" byte buffers on a single task, in sequence:
//
//   rt-stream    the GtfsRtStream's entity buffer      (4,096 B, dead once finish() is called)
//   tv-route     the buffered TransitView response     (~1-4 KB)
//   sched-stop   the buffered BusSchedules response    (<=4 KB)
//   alerts       the buffered Alerts response          (~1-3 KB)
//   bikes        the Indego scanner's feature buffer   (6,144 B)
//
// so one 6 KB reservation covers the whole cycle. Each consumer BORROWS it (GtfsRtStream::
// setEntityBuffer, indego::StatusStream::setFeatureBuffer, and this struct's own `scratch` for the
// buffered JSON bodies) and the next one takes it over.
//
// THE TWO TYPED BLOCKS BESIDE IT (0.3.2-rc1), and why they are separate from the byte scratch.
// The GTFS-RT retention block (std::vector<StopTimeUpdate>) and the TransitView vehicle list
// (std::vector<TvVehicle>) are vectors of non-trivially-destructible values: they cannot be laid
// over borrowed bytes without a custom allocator and a change to this library's public types. So
// they are their own resident vectors here, lent out the same way - `retained` to
// GtfsRtStream::setRetentionBuffer(), `tv` to fetchTransitViewAppendEx() as the destination it
// parses straight into.
//
// They were per-cycle until 0.3.2-rc1 and that decision was measured wrong. On the owner's board
// at v0.3.1 the resting largest block held above 22.5 KB for 35 healthy minutes and then
// fragmented to 3,444 B, and from that point EVERY cycle failed at the first of them: the ring
// read "pre-transit -> oom-transit" with nothing between, which is retainUpdates()' reserve() and
// nothing else. The rc1 lesson (holding memory permanently lowers the floor) still stands, so the
// cost is stated rather than waved at: `retained` is sized from the CONFIG, 8 slots per configured
// bus/trolley (stop, route) pair rather than the 32-slot default cap, which is 16 slots ~ 2.4 KB
// for the owner's two Route 17 stops; `tv` is the full kMaxTvVehicles, which since 0.3.2-rc3 is
// 16 slots ~ 2.8 KB rather than 32 ~ 5.6 KB. The thing that made 32 look necessary was that the
// parse kept a rush-hour Route 17's whole fleet; with the trip-id filter (septa.h TvFilter) it
// keeps only the vehicles a configured stop's retained realtime updates can join to, which is
// bounded by those updates and not by how busy the route is. Together about 5 KB off the resting
// floor, against removing every per-cycle contiguous request above ~1.2 KB on a cache-hit cycle.
//
// WHAT IS STILL PER-CYCLE, deliberately: the BusSchedules parse block
// (std::vector<SchedEntry>, kMaxSchedEntries * ~128 B = 3,072 B). It is asked for only on a
// schedule REFETCH - once per stop per 10 minutes, not every cycle - and 3 KB is the size this
// heap has always carried; making it resident would buy a cycle that already succeeds nothing and
// cost the floor another 3 KB.
//
// The ONE buffer the firmware still keeps separately is the transport-level BusSchedules body
// (net_poller.cpp): that layer buffers the response before transit_core sees it and then copies it
// in, so the two are briefly live together and cannot be the same vector.
//
// LIFETIME AND REENTRANCY. One cycle at a time, on one task: pollBusStops(), pollRailStops() and
// the firmware's alerts/liveness/bike helpers run one after another on the poller task and never
// nest. Passing a null PollBuffers* (the default everywhere) restores per-call buffers exactly,
// which is what the host tests and any other consumer of this library get.
struct PollBuffers {
  // The shared byte buffer. Sized for the largest single borrower (the Indego feature buffer at
  // 6,144 B); the JSON body path may grow it past that for one unusually large response.
  std::vector<uint8_t> scratch;

  // The GTFS-RT retention block, lent to GtfsRtStream::setRetentionBuffer(). Sized by
  // reserveRetention() from the configured (stop, route) pair count, not by the stream's default
  // cap.
  std::vector<StopTimeUpdate> retained;

  // The TransitView vehicle list for a whole cycle - every configured route's vehicles appended
  // into one vector, which fetchTransitViewAppendEx() parses straight into. Reserved at
  // kMaxTvVehicles so a rush-hour route cannot make it double. Since 0.3.2-rc3 that is 16 slots
  // (2,816 B) rather than 32 (5,632 B), because the parse now keeps only the vehicles a
  // configured stop's realtime updates can actually join to (septa.h TvFilter).
  std::vector<TvVehicle> tv;

  // Relevant vehicles that did not fit kMaxTvVehicles, since boot. Reported as `tv_dropped` on
  // GET /api/debug/ui. It should stay at zero: 16 slots is twice what the owner's two-stop
  // config can retain realtime updates for. A number that moves is this cap biting on a real
  // config and is the signal to raise kMaxTvVehicles - which is why the count exists rather than
  // the cap simply being generous. Only FILTERED fetches contribute: an unfiltered one (the
  // route-liveness refresh) overflows the cap on any busy route by design, and counting that
  // would drown the signal.
  uint32_t tv_dropped = 0;

  static constexpr size_t kScratchReserve = 6144;
  // How far the scratch reservation is allowed to RATCHET UP when a response body turns out to be
  // bigger than kScratchReserve (see beginCycle()). Above this, growth is still given back.
  // 8 KB and not 10 since 0.3.2-rc2: rc1 of this pass was measured with scratch_max_bytes at 7,035
  // on the owner's board, so 8 KB covers the real bodies with room to spare while capping what a
  // single outsized response can make permanent.
  static constexpr size_t kScratchMaxReserve = 8 * 1024;

  // ...and the ratchet only fires on a heap that has PROVED it can spare the bytes. 0.3.2-rc1 was
  // measured raising the reservation past 6,144 within four minutes of boot on a board whose
  // resting free8 was already 22-23 KB - i.e. the one case where growing the resident footprint is
  // the last thing wanted. 30 KB is comfortably above the resting floor this release is aiming at
  // and comfortably below a healthy board's, so a rush-hour body grows the reservation on a
  // healthy heap and is simply handed back on a struggling one.
  static constexpr size_t kScratchRatchetMinFree8 = 30 * 1024;
  // Retained updates per configured (stop, route) pair. Matches GtfsRtStream's per-pair cap, which
  // is what actually bounds how many a pair can hold, so sizing by it loses nothing.
  static constexpr size_t kRetainedPerPair = 8;

  // High-water marks, for GET /api/debug/ui. `scratch_max_bytes` is the largest body ever
  // buffered since boot - the number that says whether kScratchReserve is the right size, which
  // until 0.3.2-rc1 nothing on this device could answer. `scratch_grows` counts the times a body
  // went past the current reservation and forced a reallocation.
  uint32_t scratch_max_bytes = 0;
  uint32_t scratch_grows = 0;
  // The reservation the scratch is currently held at. Starts at kScratchReserve and ratchets up to
  // the observed high-water, bounded by kScratchMaxReserve.
  size_t scratch_reserve = kScratchReserve;

  // Call once, before the first cycle, while the heap is unfragmented.
  void reserveAll(size_t scratch_bytes = kScratchReserve);

  // Sizes the retention block for `pairs` configured bus/trolley (stop, route) pairs. Idempotent,
  // and it only ever grows: a config change that adds a stop pays one contiguous request at the
  // top of the next cycle, where the largest free block is at its best. `pairs == 0` (a
  // subway/rail-only config) reserves nothing.
  void reserveRetention(size_t pairs);

  // Call at the top of each cycle. Clears without releasing. Growth beyond the current reservation
  // is KEPT (the reservation ratchets up to it) only when it is under kScratchMaxReserve AND
  // `free8_at_poll_start` is at least kScratchRatchetMinFree8; otherwise it is released. Doing this
  // at poll-start rather than at cycle end means any replacement block is asked for at the point in
  // the cycle where the largest free block is at its healthiest.
  //
  // `free8_at_poll_start` is the caller's MALLOC_CAP_8BIT reading (this library has no Arduino and
  // cannot take one). The default says "assume plenty", which is what every non-firmware caller and
  // every host test that is not about the ratchet itself wants.
  void beginCycle(size_t free8_at_poll_start = static_cast<size_t>(-1));

  // Records that a body reached `bytes`, for the high-water marks above. Called by fetchBuffered().
  void noteBodyBytes(size_t bytes);
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
// Constructed with a PollBuffers*, the body buffer is that struct's shared scratch instead of a
// function local, so a cycle asks the allocator for it once rather than once per fetch (see
// PollBuffers above). Nothing else changes: the same bodies are buffered under the same cap and
// the same out-parameter contract holds - a fetch that fails leaves `*out` untouched.
class SeptaSource : public TransitSource {
 public:
  SeptaSource() = default;
  explicit SeptaSource(PollBuffers* buffers) : buffers_(buffers) {}

  int fetchRealtime(GtfsRtStream& stream, HttpGet http) override;
  int fetchSchedule(const std::string& stop_id, std::vector<SchedEntry>* out,
                     HttpGet http) override;
  int fetchAlerts(Mode mode, const std::string& route, std::vector<transit::Alert>* out,
                   HttpGet http) override;

  // TransitView vehicle positions and Regional Rail Arrivals have no legacy status-code form:
  // neither is part of the portable TransitSource interface (no other agency modeled in this
  // project has an equivalent endpoint), so there was nothing to keep them for and nothing ever
  // called them - see fetchTransitViewEx()/fetchRailArrivalsEx() below.

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

  // The same fetch, APPENDING this route's vehicles to `*out` and parsing straight into it, so a
  // poll cycle holds ONE TvVehicle vector instead of a per-route one plus the accumulated one
  // (0.3.2-rc1). `*out` is left exactly as it was if the fetch or the parse fails, which is the
  // same contract fetchTransitViewEx() has always had. kMaxTvVehicles applies to the total.
  //
  // `filter` (septa.h TvFilter) decides which vehicles are built at all; the default keeps every
  // one, which is what refreshRouteLiveness() and the host tests want. When a PollBuffers is
  // attached AND a filter is installed, relevant vehicles that did not fit the cap are added to
  // PollBuffers::tv_dropped; an unfiltered call never adds to it, because on such a call the cap
  // says nothing about relevance (see the .cpp).
  FetchOutcome fetchTransitViewAppendEx(const std::string& route, std::vector<TvVehicle>* out,
                                         HttpGetEx http, const TvFilter& filter = TvFilter());
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
// `buffers`, when non-null, is the caller's shared scratch buffer (PollBuffers above): the
// TripUpdates decoder borrows it for its entity buffer and then hands it on to each buffered JSON
// response. Null (the default) behaves exactly as before.
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
