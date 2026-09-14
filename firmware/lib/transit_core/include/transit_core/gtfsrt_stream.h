// Streaming GTFS-Realtime TripUpdates decoder. Arduino-independent (host + ESP32).
// See DESIGN.md 4.2. Hand-rolled protobuf wire format: no nanopb/protobuf-lite dependency,
// because the only thing we need is TripUpdates and a full protobuf runtime would cost far more
// flash/RAM than this device has to spare.
//
// Wire-format fields decoded (field numbers are the GTFS-RT proto's, not ours):
//   FeedMessage      1 header (FeedHeader), 2 entity (repeated FeedEntity)
//   FeedHeader       3 timestamp (uint64)
//   FeedEntity       1 id (string, decoded but unused), 3 trip_update (TripUpdate)
//   TripUpdate       1 trip (TripDescriptor), 2 stop_time_update (repeated StopTimeUpdate),
//                    3 vehicle (VehicleDescriptor); fields 4 (timestamp) and 5 (delay) are
//                    trip-level and are parsed-and-skipped (not surfaced; see .cpp).
//   TripDescriptor   1 trip_id (string), 5 route_id (string), 6 direction_id (varint);
//                    4 schedule_relationship (varint) is parsed-and-skipped: DESIGN.md's
//                    "SKIPPED means detour" refers to the STOP's own schedule_relationship
//                    (StopTimeUpdate field 5), which is a different enum with overlapping
//                    numeric values, so the trip-level one is never merged into the output.
//   StopTimeUpdate   1 stop_sequence (varint), 4 stop_id (string), 2 arrival (StopTimeEvent),
//                    3 departure (StopTimeEvent), 5 schedule_relationship (varint)
//   StopTimeEvent    1 delay (varint, sign-extended int32), 2 time (varint, int64 epoch);
//                    3 uncertainty is parsed-and-skipped.
//   VehicleDescriptor 1 id (string) -> vehicle_id; 2 label is parsed-and-skipped.
//
// Streaming discipline (the reason this file exists instead of just calling a one-shot decode
// on a fully-buffered body): the SEPTA bus TripUpdates feed is ~150 KB, far more than this
// device's ~100-160 KB usable heap. push() consumes the HTTP body incrementally, in whatever
// chunk sizes the caller has on hand (down to 1 byte at a time), and never buffers more than
// ONE FeedEntity at a time (bounded by max_entity_bytes, default 4096; the real feed's largest
// observed entity is ~2.3 KB). Entities bigger than the cap are skipped byte-for-byte (the
// length-delimited body is consumed and discarded without allocating for it) and counted in
// entitiesSkippedTooLarge(), without disturbing the parse of subsequent entities.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace transit {

// One (trip, stop) prediction extracted from a TripUpdate's stop_time_update[] entry, already
// carrying the parent trip's identifying fields so callers don't need to re-join anything.
struct StopTimeUpdate {
  std::string trip_id;
  std::string route_id;
  std::string vehicle_id;
  std::string stop_id;
  int direction_id = -1;            // GTFS direction_id; -1 if the feed didn't include one
  uint32_t stop_sequence = 0;
  int64_t arrival_time = 0;         // epoch seconds; meaningful only if has_arrival_time
  int64_t departure_time = 0;       // epoch seconds; meaningful only if has_departure_time
  int32_t arrival_delay = 0;        // seconds; meaningful only if has_arrival_time
  int32_t departure_delay = 0;      // seconds; meaningful only if has_departure_time
  uint8_t schedule_relationship = 0;  // StopTimeUpdate.ScheduleRelationship: 0=SCHEDULED, 1=SKIPPED, 2=NO_DATA, 3=UNSCHEDULED
  bool has_arrival_time = false;
  bool has_departure_time = false;
};

// Streaming decoder for one GTFS-RT FeedMessage of TripUpdates.
//
// Memory footprint (fixed, independent of feed size): sizeof(GtfsRtStream) is dominated by
// `max_entity_bytes` (default 4096, reserved once in the constructor and reused for every
// entity) plus a small (<128 byte) fixed header-decode buffer, the route/stop filter vectors
// (proportional to configured routes/stops - a handful of short strings, well under 1 KB for
// any realistic config), and the std::function callback. It never allocates in proportion to
// the feed body: a ~150 KB SEPTA TripUpdates response is processed with the same footprint as
// a 1 KB one. Total: roughly max_entity_bytes + ~1 KB overhead, e.g. ~5 KB at the default cap.
//
// Usage: construct once per poll, optionally set filters and the update callback, then feed
// HTTP body bytes to push() as they arrive (any chunk size, including 1 byte at a time - this
// is exercised by tests), and call finish() when the body is complete. Not thread-safe; not
// reentrant (do not call push() from within the onUpdate callback).
class GtfsRtStream {
 public:
  // max_entity_bytes bounds the largest FeedEntity this decoder will fully buffer and decode.
  // Larger entities are skipped (see entitiesSkippedTooLarge()) rather than rejected outright,
  // so one oversized entity never aborts the whole feed.
  explicit GtfsRtStream(size_t max_entity_bytes = 4096);

  // Restricts decoded updates to these route_ids (TripDescriptor.route_id). An empty vector
  // (the default) means "all routes". Call before push(); not safe to change mid-stream.
  void setRouteFilter(std::vector<std::string> routes);

  // Restricts decoded updates to these stop_ids (StopTimeUpdate.stop_id). An empty vector
  // (the default) means "all stops". Call before push(); not safe to change mid-stream.
  void setStopFilter(std::vector<std::string> stops);

  // Registers the callback invoked once per StopTimeUpdate that survives the route/stop
  // filters. Invoked synchronously from within push(), in wire order.
  void onUpdate(std::function<void(const StopTimeUpdate&)> cb);

  // Feeds the next chunk of the HTTP response body. May be called any number of times with any
  // chunk sizes (including 1 byte); the parse state persists across calls. Returns false if the
  // stream has hit an unrecoverable framing error (an unsupported wire type at the top level);
  // once false, the stream should be discarded rather than fed further.
  bool push(const uint8_t* data, size_t len);

  // Signals end of input. Any entity or header left mid-parse (a truncated body) is simply
  // discarded; no callback fires for it. Safe to call even if push() was never called.
  void finish();

  // Total FeedEntity fields encountered at the top level, decoded or not.
  uint32_t entitiesSeen() const;

  // Subset of entitiesSeen() that were skipped for exceeding max_entity_bytes.
  uint32_t entitiesSkippedTooLarge() const;

  // FeedHeader.timestamp (epoch seconds), or 0 if no header has been parsed yet.
  int64_t headerTimestamp() const;

 private:
  enum class State : uint8_t {
    kTag,          // reading the top-level field's tag varint
    kLen,          // reading the top-level field's length varint (always wiretype 2 fields)
    kHeaderBody,   // buffering FeedHeader bytes
    kEntityBody,   // buffering one FeedEntity's bytes
    kSkipVarint,   // skipping an unknown/oversized varint field
    kSkipFixed,    // skipping a fixed32/fixed64 field, or the tail of an oversized/unknown
                   // length-delimited field
  };

  void feedByte(uint8_t b);
  void decodeHeader(const uint8_t* buf, size_t len);
  void decodeEntity(const uint8_t* buf, size_t len);
  void decodeTripUpdate(const uint8_t* buf, size_t len);
  void decodeTripDescriptor(const uint8_t* buf, size_t len, std::string* trip_id,
                             std::string* route_id, int* direction_id);
  void decodeVehicleDescriptor(const uint8_t* buf, size_t len, std::string* vehicle_id);
  void decodeStopTimeUpdate(const uint8_t* buf, size_t len, const std::string& trip_id,
                             const std::string& route_id, const std::string& vehicle_id,
                             int direction_id);
  void decodeStopTimeEvent(const uint8_t* buf, size_t len, int32_t* delay, int64_t* time,
                            bool* has_time);
  bool routeAllowed(const std::string& route_id) const;
  bool stopAllowed(const std::string& stop_id) const;

  size_t max_entity_bytes_;
  std::vector<std::string> route_filter_;
  std::vector<std::string> stop_filter_;
  std::function<void(const StopTimeUpdate&)> on_update_;

  State state_ = State::kTag;
  bool error_ = false;

  // Tag/length varint accumulation, shared across states since only one is ever in flight.
  uint64_t varint_value_ = 0;
  uint8_t varint_shift_ = 0;
  uint32_t cur_field_ = 0;
  uint8_t cur_wiretype_ = 0;

  uint64_t skip_remaining_ = 0;  // bytes left to discard (kSkipFixed) for the current field

  static constexpr size_t kHeaderBufCap = 128;
  uint8_t header_buf_[kHeaderBufCap];
  size_t header_fill_ = 0;
  size_t header_target_ = 0;

  std::vector<uint8_t> entity_buf_;
  size_t entity_target_ = 0;

  uint32_t entities_seen_ = 0;
  uint32_t entities_skipped_too_large_ = 0;
  int64_t header_timestamp_ = 0;
};

}  // namespace transit
