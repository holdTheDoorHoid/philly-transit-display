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
