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
//   TripDescriptor   1 trip_id (string), 5 route_id (string), 6 direction_id (varint),
//                    4 schedule_relationship (varint) -> StopTimeUpdate::trip_schedule_relationship.
//                    That trip-level enum is a DIFFERENT enum from the stop-level one with
//                    overlapping numeric values (TripRel vs StopRel below), so the two are kept in
//                    separate fields and never compared to each other. DESIGN.md 4.2's "SKIPPED
//                    means detour" is the STOP-level value (StopTimeUpdate field 5); the
//                    trip-level CANCELED is a different statement ("this trip is not running at
//                    all") and mergeStop() acts on it differently.
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
//
// That per-entity cap bounds the decode, NOT the total kept: a caller that appends every matching
// update to a vector of its own grows with the number of MATCHING entities, which is exactly as
// unbounded as the feed. retainUpdates() gives the stream its own pre-reserved, hard-capped
// buffer for that instead (see below), so "how much memory can this feed cost us" has one answer
// set at construction time rather than one per caller.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace transit {

// StopTimeUpdate.ScheduleRelationship - what the feed says about THIS STOP on this trip.
enum class StopRel : uint8_t {
  Scheduled = 0,    // a normal prediction
  Skipped = 1,      // the vehicle will not serve this stop (detour) - DESIGN.md 4.2 says show it
  NoData = 2,       // explicitly "no realtime for this stop": NOT a prediction, and not a
                    // prediction of zero either - the schedule is all there is
  Unscheduled = 3,
};

// TripDescriptor.ScheduleRelationship - what the feed says about the WHOLE TRIP. Deliberately a
// separate enum from StopRel: the numbers overlap but mean different things (1 is SKIPPED at the
// stop level and ADDED at the trip level), and conflating them is how a cancelled trip ends up
// displayed as an ordinary one.
enum class TripRel : uint8_t {
  Scheduled = 0,
  Added = 1,
  Unscheduled = 2,
  Canceled = 3,     // the trip is not running: nothing about it should be displayed
  Duplicated = 5,
  Deleted = 6,      // like Canceled, but "do not show at all" rather than "show as cancelled"
};

// One (trip, stop) prediction extracted from a TripUpdate's stop_time_update[] entry, already
// carrying the parent trip's identifying fields so callers don't need to re-join anything.
//
// Identifier strings are truncated to kMaxIdentifierChars on decode (see GtfsRtStream's
// identifiersTruncated()): the wire format puts no limit on them and this struct is retained in
// vectors, so an agency-controlled 1 MB trip_id must not become 1 MB of heap per update.
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
  uint8_t schedule_relationship = 0;       // raw StopTimeUpdate.ScheduleRelationship; see StopRel
  uint8_t trip_schedule_relationship = 0;  // raw TripDescriptor.ScheduleRelationship; see TripRel
  int64_t feed_timestamp = 0;       // FeedHeader.timestamp of the feed this came from (epoch
                                     // seconds), 0 if the feed published none. This is when the
                                     // AGENCY produced the prediction, which is not when we
                                     // fetched it - a cached/replayed body looks identical
                                     // otherwise (DESIGN.md 4.7, merge.h's staleness rule).
  bool has_arrival_time = false;
  bool has_departure_time = false;

  // The raw values above kept verbatim for fidelity; these are how the rest of the code asks.
  bool stopSkipped() const { return schedule_relationship == static_cast<uint8_t>(StopRel::Skipped); }
  bool stopNoData() const { return schedule_relationship == static_cast<uint8_t>(StopRel::NoData); }
  bool tripCanceled() const {
    return trip_schedule_relationship == static_cast<uint8_t>(TripRel::Canceled) ||
           trip_schedule_relationship == static_cast<uint8_t>(TripRel::Deleted);
  }
  bool hasTime() const { return has_arrival_time || has_departure_time; }
  // Predicted time for this stop: arrival if the feed gave one, else departure, else 0.
  int64_t predictedTime() const {
    return has_arrival_time ? arrival_time : (has_departure_time ? departure_time : 0);
  }
};

// How a feed body ended, as reported by GtfsRtStream::finish(). The protobuf wire format has no
// terminator, so "the body stopped arriving" and "the body is complete" are indistinguishable
// without this: a connection cut halfway through a 150 KB feed used to look exactly like a small
// feed, and every stop it should have carried simply came back schedule-only with no error.
enum class FeedStatus : uint8_t {
  Complete,   // the last byte landed exactly on a top-level field boundary
  Truncated,  // input ended mid-header, mid-entity or mid-varint: the body was cut short
  Malformed,  // top-level framing was invalid (push() had already returned false); unusable
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
// retainUpdates() adds a second fixed cost on top: max_total * sizeof(StopTimeUpdate) plus the
// ids' own bytes, each id bounded by kMaxIdentifierChars - about 8-12 KB at the 64-update
// default on ESP32. Still constant, and still independent of how large the feed is.
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

  // Puts the stream back into its just-constructed state - parse state, filters, callback,
  // counters, header timestamp and retention all cleared - while KEEPING the capacity of the
  // entity buffer and the retention block. `max_entity_bytes` re-sizes the entity buffer's cap
  // the way the constructor would, growing the reservation if it is larger than what is already
  // held and leaving the existing capacity alone if it is not.
  //
  // This exists so one stream object can decode a new feed every poll cycle instead of asking the
  // allocator for its buffers again (DESIGN.md SS5, "the poll working set").
  void reset(size_t max_entity_bytes);

  // Decode entities into a buffer the CALLER owns, instead of one of this object's own.
  //
  // The entity buffer is a few kilobytes that are live only while push() is running: once
  // finish() has been called nothing reads it again. On the ESP32 that makes it the ideal thing
  // to share - the same bytes serve as this decoder's entity buffer during the TripUpdates fetch,
  // as the buffered-JSON-response buffer for every fetch after it, and as the Indego feed
  // scanner's one-feature buffer later in the cycle, so ONE reservation covers a whole poll
  // instead of four (DESIGN.md SS5). `borrowed` must outlive the decode; passing nullptr (the
  // default state) puts the stream back on its own buffer.
  //
  // The borrow is a `std::vector<uint8_t>*` and not a pointer to raw bytes on purpose: the vector
  // may reallocate while somebody else is using it for something larger, and a borrow of the
  // container rather than of its storage survives that.
  void setEntityBuffer(std::vector<uint8_t>* borrowed);

  // Retain decoded updates into a vector the CALLER owns, instead of this object's own.
  //
  // WHY THIS EXISTS (0.3.2-rc1). retainUpdates() reserves max_total slots, and this object is a
  // local in pollBusStops() - so that reservation was one contiguous request of
  // max_total * sizeof(StopTimeUpdate) (about 4.9 KB at 32 slots on the ESP32) EVERY poll cycle.
  // On the owner's board at v0.3.1 it is the request that failed once the largest free block had
  // fragmented to 3,444 B: the ring read "pre-transit -> oom-transit" with no stage in between,
  // which is this allocation and nothing else. Handing the stream a vector that lives across
  // cycles makes retainUpdates()' reserve() a no-op from the second cycle on.
  //
  // Unlike the entity buffer this one is NOT shareable with the byte scratch - a
  // std::vector<StopTimeUpdate> is a vector of non-trivially-destructible values and cannot be laid
  // over borrowed bytes without a custom allocator - so it is its own resident block. Size it for
  // what the caller will actually ask retainUpdates() to keep, not for the default cap.
  //
  // The borrow is of the CONTAINER, like setEntityBuffer(), so a reserve() by the stream cannot
  // dangle it. `borrowed` must outlive the decode AND every read of retained(); passing nullptr
  // (the default state) puts the stream back on its own vector.
  void setRetentionBuffer(std::vector<StopTimeUpdate>* borrowed);

  // Restricts decoded updates to these route_ids (TripDescriptor.route_id). An empty vector
  // (the default) means "all routes". Call before push(); not safe to change mid-stream.
  void setRouteFilter(std::vector<std::string> routes);

  // Restricts decoded updates to these stop_ids (StopTimeUpdate.stop_id). An empty vector
  // (the default) means "all stops". Call before push(); not safe to change mid-stream.
  void setStopFilter(std::vector<std::string> stops);

  // Registers the callback invoked once per StopTimeUpdate that survives the route/stop
  // filters. Invoked synchronously from within push(), in wire order.
  void onUpdate(std::function<void(const StopTimeUpdate&)> cb);

  // Turns on the stream's own bounded retention buffer, so a caller does not have to collect
  // updates into a vector of its own (the way to run out of heap on a feed with many matching
  // entities: the 4 KB per-entity cap bounds ONE entity, not the total kept).
  //
  //   max_total          hard cap on retained StopTimeUpdates across all stops/routes
  //   max_per_stop_route hard cap per (stop_id, route_id) pair
  //
  // Both are allocated up front (reserve(max_total), no growth afterwards). When a cap is
  // reached, the retained set keeps the NEAREST upcoming predictions: the farthest-future
  // retained entry is evicted for a nearer newcomer, and a newcomer farther out than everything
  // retained is dropped. A display only ever shows the next few arrivals, so "drop the far
  // future" loses nothing while making the footprint a constant. Updates with no time of their
  // own (SKIPPED/NO_DATA) rank by the feed's header timestamp, i.e. as near-term, because they
  // are statements about imminent service. Call before push(). Every drop is counted in
  // updatesDroppedByCap().
  void retainUpdates(size_t max_total = kDefaultMaxRetainedUpdates,
                      size_t max_per_stop_route = kDefaultMaxPerStopRoute);

  // The retained updates, in no particular order (eviction overwrites slots). Empty unless
  // retainUpdates() was called.
  const std::vector<StopTimeUpdate>& retained() const;

  // Feeds the next chunk of the HTTP response body. May be called any number of times with any
  // chunk sizes (including 1 byte); the parse state persists across calls. Returns false if the
  // stream has hit an unrecoverable framing error (an unsupported wire type at the top level);
  // once false, the stream should be discarded rather than fed further.
  bool push(const uint8_t* data, size_t len);

  // Signals end of input and reports how the body ended (see FeedStatus). Any entity or header
  // left mid-parse is discarded; no callback fires for it, but the return value now says so
  // instead of leaving the caller to assume the feed was simply small. Safe to call even if
  // push() was never called (an empty body is Complete - a valid, if useless, FeedMessage).
  FeedStatus finish();

  // The same value finish() returned, queryable afterwards. Before finish(), reports what the
  // status would be if the body ended right now.
  FeedStatus feedStatus() const;

  // Total FeedEntity fields encountered at the top level, decoded or not.
  uint32_t entitiesSeen() const;

  // Subset of entitiesSeen() that were skipped for exceeding max_entity_bytes.
  uint32_t entitiesSkippedTooLarge() const;

  // Entities that produced at least one StopTimeUpdate past both filters.
  uint32_t entitiesMatched() const;

  // Entities whose own bytes did not decode as a well-formed nested message (a length-delimited
  // field that ran past its declared end, an unsupported wire type inside an entity, ...). Such
  // an entity is skipped; the rest of the feed still decodes, so this does NOT make finish()
  // report Malformed - it is counted separately so callers can report partial corruption.
  uint32_t entitiesMalformed() const;

  // Total StopTimeUpdates that passed the route and stop filters (whether retained or not).
  uint32_t updatesMatched() const;

  // Matched updates the retention caps refused to keep. Always 0 unless retainUpdates() is on.
  uint32_t updatesDroppedByCap() const;

  // Identifier strings shortened to kMaxIdentifierChars while decoding.
  uint32_t identifiersTruncated() const;

  // FeedHeader.timestamp (epoch seconds), or 0 if no header has been parsed yet. Also copied
  // onto every StopTimeUpdate as feed_timestamp, so a merge that only sees the updates can still
  // judge the feed's age.
  int64_t headerTimestamp() const;

  // Longest identifier (trip/route/vehicle/stop id) retained; longer values are truncated.
  static constexpr size_t kMaxIdentifierChars = 48;
  // Defaults for retainUpdates(). 32 updates is 4x what this device's 8-stop maximum
  // (DESIGN.md 6) can display, and 8 per (stop, route) is 2x the 4-row panel maximum. They were
  // 64/12 for one day: reserve(64) is a ~10 KB contiguous block on the ESP32 (sizeof
  // StopTimeUpdate is ~150 B there), and on the owner's board the largest free block between
  // polls is 10-20 KB, so the reservation itself threw bad_alloc in the poller (2026-09-15,
  // device suite). Halving it keeps the block under ~5 KB, the size the unbounded vector used to
  // reach anyway on a normal day.
  static constexpr size_t kDefaultMaxRetainedUpdates = 32;
  static constexpr size_t kDefaultMaxPerStopRoute = 8;

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
  void decodeTripUpdate(const uint8_t* buf, size_t len, bool* matched);
  void decodeTripDescriptor(const uint8_t* buf, size_t len, std::string* trip_id,
                             std::string* route_id, int* direction_id, uint8_t* trip_relationship);
  void decodeVehicleDescriptor(const uint8_t* buf, size_t len, std::string* vehicle_id);
  void decodeStopTimeUpdate(const uint8_t* buf, size_t len, const std::string& trip_id,
                             const std::string& route_id, const std::string& vehicle_id,
                             int direction_id, uint8_t trip_relationship, bool* matched);
  void decodeStopTimeEvent(const uint8_t* buf, size_t len, int32_t* delay, int64_t* time,
                            bool* has_time);
  bool routeAllowed(const std::string& route_id) const;
  bool stopAllowed(const std::string& stop_id) const;
  void assignCapped(std::string* dst, const uint8_t* data, size_t len);
  void retain(StopTimeUpdate&& u);

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

  // The entity buffer is reached through entityBuf() so it can be the caller's (setEntityBuffer)
  // rather than own_entity_buf_. own_entity_buf_ stays empty and unreserved while a borrow is in
  // place, so a borrowing caller pays for one buffer and not two.
  std::vector<uint8_t>& entityBuf() { return borrowed_entity_buf_ != nullptr ? *borrowed_entity_buf_ : own_entity_buf_; }
  std::vector<uint8_t> own_entity_buf_;
  std::vector<uint8_t>* borrowed_entity_buf_ = nullptr;
  size_t entity_target_ = 0;

  // Reached through retainedBuf() so it can be the caller's (setRetentionBuffer) rather than
  // own_retained_. own_retained_ is emptied and released while a borrow is in place, so a
  // borrowing caller pays for one block and not two.
  std::vector<StopTimeUpdate>& retainedBuf() { return borrowed_retained_ != nullptr ? *borrowed_retained_ : own_retained_; }
  const std::vector<StopTimeUpdate>& retainedBuf() const { return borrowed_retained_ != nullptr ? *borrowed_retained_ : own_retained_; }
  std::vector<StopTimeUpdate> own_retained_;
  std::vector<StopTimeUpdate>* borrowed_retained_ = nullptr;
  size_t max_retained_ = 0;  // 0 = retention off (callback-only use)
  size_t max_per_stop_route_ = 0;

  uint32_t entities_seen_ = 0;
  uint32_t entities_skipped_too_large_ = 0;
  uint32_t entities_matched_ = 0;
  uint32_t entities_malformed_ = 0;
  uint32_t updates_matched_ = 0;
  uint32_t updates_dropped_by_cap_ = 0;
  uint32_t identifiers_truncated_ = 0;
  int64_t header_timestamp_ = 0;
};

}  // namespace transit
