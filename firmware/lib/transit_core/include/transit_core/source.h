// Agency-agnostic real-time source abstraction (DESIGN.md 11) and the HTTP/cache seams
// transit_core needs to be testable on host without any network or Arduino dependency.
// Arduino-independent (host + ESP32); header-only declarations, no state of its own.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "transit_core/gtfsrt_stream.h"
#include "transit_core/model.h"
#include "transit_core/septa.h"

namespace transit {

// Streaming HTTP GET the platform provides. Implementations must call onData zero or more times
// with successive chunks of the response body AS THEY ARRIVE - never buffer the whole body
// themselves before the first call - and return the final HTTP status code (0 if the request
// could not even be sent, e.g. DNS/TLS/connect failure). onData returns false to ask the
// transfer to stop early; a well-behaved implementation stops calling it and returns promptly.
//
// Retry/backoff (DESIGN.md 4.4/4.7: BusSchedules needs it; the poll loop needs it on any
// endpoint) is the HttpGet implementation's responsibility, not transit_core's - by the time
// transit_core sees a status code, retries (if any) have already happened. This is also why
// SeptaSource's fetch methods attempt to parse the body whenever one came back, regardless of
// status: SEPTA has been observed returning a perfectly valid-shaped BusSchedules body on a
// non-200 status (NOTES.md), so the status code alone is not a reliable "was there usable data"
// signal.
//
// transit_core never talks sockets/TLS itself (that's firmware/src/app/http_fetch.*, owned by
// the firmware-skeleton work); native tests substitute a fake that replays fixture bytes - see
// test/test_core/test_septa_source.cpp.
using HttpGet =
    std::function<int(const std::string& url, std::function<bool(const uint8_t*, size_t)> onData)>;

// What a transport can tell us beyond the status code. A status code alone cannot answer the one
// question that matters for a streamed body: did we receive ALL of it? A connection dropped
// halfway through the 150 KB TripUpdates feed still reports 200, and the decoder then sees a
// short-but-syntactically-fine feed - the stops the missing half would have filled come back
// empty and "successful". `complete` is the transport saying it reached the end of the body
// (Content-Length satisfied, or a clean chunked/EOF termination), not a guess.
struct FetchResult {
  int status = 0;        // HTTP status code; 0 if the request could not be made at all
  bool complete = false; // the whole body arrived
  bool aborted = false;  // WE stopped it: onData returned false (e.g. a size cap was hit)
  size_t bytes = 0;      // body bytes handed to onData
};

// Streaming GET with completeness reporting. Same contract as HttpGet otherwise. This is what
// SeptaSource, pollBusStops() and pollRailStops() take; HttpGet remains for callers (and the
// TransitSource interface) that have nothing better to report.
using HttpGetEx = std::function<FetchResult(const std::string& url,
                                             std::function<bool(const uint8_t*, size_t)> onData)>;

// Wraps a plain HttpGet as an HttpGetEx. The wrapped transport cannot report completeness, so
// this assumes the body was complete unless onData refused a chunk - the best available reading,
// and the behaviour every caller had before FetchResult existed. Used by the legacy overloads
// and by tests whose fake transport replays a whole fixture.
inline HttpGetEx adaptHttpGet(HttpGet http) {
  return [http](const std::string& url,
                 std::function<bool(const uint8_t*, size_t)> onData) -> FetchResult {
    FetchResult r;
    bool refused = false;
    r.status = http(url, [&](const uint8_t* data, size_t len) {
      if (!onData(data, len)) {
        refused = true;
        return false;
      }
      r.bytes += len;
      return true;
    });
    r.aborted = refused;
    r.complete = !refused;
    return r;
  };
}

// ---------------------------------------------------------------------------------------------
// Optional instrumentation hook (diag branch, 2026-09-17).
//
// pollBusStops() is where three of the poll cycle's four biggest allocations live - the 4,096 B
// GTFS-RT entity buffer, the ~4,800 B retained-updates buffer and the per-stop BusSchedules body
// with its 3,072 B SchedEntry reserve - so a heap trace that only samples either side of the whole
// call cannot say which of them is the one that stops fitting. This lets the firmware sample
// INSIDE the call without transit_core gaining an Arduino, FreeRTOS or heap dependency.
//
// A plain function pointer, deliberately, and not a std::function: a std::function member would
// allocate for any capture over 8 bytes, and an instrument must not allocate on the path it is
// measuring. Null everywhere nothing installs one - the host tests, the simulator - so the cost
// where it is not used is one null check per stage.
enum PollTraceStage : int {
  kPollTraceRtStream = 0,     // the GTFS-RT TripUpdates stream has been fetched and filtered
  kPollTraceTransitView = 1,  // one route's TransitView fetch+parse has finished
  kPollTraceSchedStop = 2,    // one stop's BusSchedules lookup has finished (cache hit or fetch)
  kPollTraceMerge = 3,        // every stop has been merged into the Snapshot
};
using PollTraceHook = void (*)(int stage);
inline PollTraceHook g_poll_trace_hook = nullptr;
inline void setPollTraceHook(PollTraceHook hook) { g_poll_trace_hook = hook; }
inline void pollTrace(int stage) {
  if (g_poll_trace_hook != nullptr) g_poll_trace_hook(stage);
}

// Caches BusSchedules results per stop_id (DESIGN.md 4.7: cache 10 minutes, "also on config
// change"). transit_core defines only the interface - it has no clock or persistent storage of
// its own. The Arduino glue layer supplies a concrete implementation (e.g. backed by millis()
// and a small in-RAM map keyed by stop_id, at most 8 entries per DESIGN.md 6's stop limit);
// native tests use a trivial in-memory or always-miss fake.
//
// Memory: this interface itself costs one vtable pointer (4 bytes on ESP32); the concrete
// implementation's footprint is its own concern (a suggested in-RAM map of <=8 stop_ids x a
// handful of SchedEntry each is on the order of a few hundred bytes to ~1-2 KB total).
class ScheduleCache {
 public:
  virtual ~ScheduleCache() = default;

  // Returns true and fills `out` if a fresh-enough cached schedule exists for stop_id; returns
  // false (leaving `out` untouched) on a cache miss or an expired entry.
  virtual bool get(const std::string& stop_id, std::vector<SchedEntry>* out) = 0;

  // Replaces the cached schedule for stop_id with `entries` and resets its freshness clock.
  virtual void put(const std::string& stop_id, const std::vector<SchedEntry>& entries) = 0;

  // Like put(), for a schedule that still looked implausible after every fetch attempt (see
  // fetchPlausibleSchedule() in septa_source.h: SEPTA sometimes answers with the wrong service
  // day). Implementations should keep such entries for a much shorter time so the next poll gets
  // another chance at a good answer. Default: same as put().
  virtual void putSuspect(const std::string& stop_id, const std::vector<SchedEntry>& entries) {
    put(stop_id, entries);
  }
};

// Agency-agnostic real-time source interface (DESIGN.md 11). SeptaSource (septa_source.h) is
// the only v1 implementation. Porting to an agency that publishes GTFS-RT TripUpdates plus some
// per-stop schedule endpoint should only need a new class implementing this interface (plus a
// config `agency` field elsewhere) - gtfsrt_stream.h and merge.h are already agency-agnostic.
//
// Memory: the interface itself is stateless (one vtable pointer, 4 bytes on ESP32); see
// septa_source.h for SeptaSource's own footprint.
class TransitSource {
 public:
  virtual ~TransitSource() = default;

  // Streams this agency's GTFS-RT TripUpdates feed through `stream` (already configured with
  // its route/stop filters and onUpdate callback by the caller) using `http`, then calls
  // stream.finish(). Returns the HTTP status code (0 on transport failure).
  virtual int fetchRealtime(GtfsRtStream& stream, HttpGet http) = 0;

  // Fetches this agency's schedule for one stop. On success, *out holds the parsed entries; on
  // failure (transport or parse), *out is left unmodified so a caller can keep using a
  // previous/cached value. Returns the HTTP status code.
  virtual int fetchSchedule(const std::string& stop_id, std::vector<SchedEntry>* out,
                             HttpGet http) = 0;

  // Fetches current alerts relevant to one configured route/line (mode disambiguates which
  // alert-feed naming convention applies - see septa_source.h's alertRouteIdFor()). Same
  // out-param contract as fetchSchedule. Returns the HTTP status code (0 if no request could be
  // made at all, e.g. an unrecognized rail line code).
  virtual int fetchAlerts(Mode mode, const std::string& route, std::vector<transit::Alert>* out,
                           HttpGet http) = 0;
};

}  // namespace transit
