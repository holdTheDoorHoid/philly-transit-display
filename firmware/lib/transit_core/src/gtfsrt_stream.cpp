#include "transit_core/gtfsrt_stream.h"

#include <algorithm>

namespace transit {

namespace {

// A view of one decoded protobuf field within a fully-buffered message. For wiretype 0
// (varint) `varint` holds the value; for wiretypes 1/2/5 (fixed64/length-delimited/fixed32)
// `data`/`len` point into the caller-owned buffer (no copy).
struct FieldRef {
  uint32_t field = 0;
  uint8_t wiretype = 0;
  uint64_t varint = 0;
  const uint8_t* data = nullptr;
  size_t len = 0;
};

// Reads one varint from buf[offset..], advancing offset. Returns false if the buffer runs out
// or the varint exceeds 10 bytes (which would overflow a 64-bit value) - either way, malformed
// input, and the caller should stop parsing this message.
bool readVarint(const uint8_t* buf, size_t len, size_t& offset, uint64_t& out) {
  uint64_t result = 0;
  uint8_t shift = 0;
  while (true) {
    if (offset >= len || shift >= 70) return false;
    uint8_t b = buf[offset++];
    result |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80)) {
      out = result;
      return true;
    }
    shift += 7;
  }
}

// Reads the next top-level field of a fully-buffered message. Returns false when `offset`
// reaches `len` (normal end of message) or the field is malformed/truncated.
bool readNextField(const uint8_t* buf, size_t len, size_t& offset, FieldRef& out) {
  if (offset >= len) return false;
  uint64_t tag = 0;
  if (!readVarint(buf, len, offset, tag)) return false;
  out.field = static_cast<uint32_t>(tag >> 3);
  out.wiretype = static_cast<uint8_t>(tag & 7);
  switch (out.wiretype) {
    case 0:  // varint
      out.data = nullptr;
      out.len = 0;
      return readVarint(buf, len, offset, out.varint);
    case 1:  // fixed64
      if (offset + 8 > len) return false;
      out.data = buf + offset;
      out.len = 8;
      offset += 8;
      return true;
    case 2: {  // length-delimited
      uint64_t l = 0;
      if (!readVarint(buf, len, offset, l)) return false;
      if (l > len - offset) return false;  // truncated
      out.data = buf + offset;
      out.len = static_cast<size_t>(l);
      offset += out.len;
      return true;
    }
    case 5:  // fixed32
      if (offset + 4 > len) return false;
      out.data = buf + offset;
      out.len = 4;
      offset += 4;
      return true;
    default:
      return false;  // wiretypes 3/4 (deprecated groups) are not supported
  }
}

// Sign-extends a protobuf-standard (non-zigzag) int32 that was decoded into a 64-bit varint.
int32_t toInt32(uint64_t v) { return static_cast<int32_t>(static_cast<int64_t>(v)); }

}  // namespace

GtfsRtStream::GtfsRtStream(size_t max_entity_bytes) : max_entity_bytes_(max_entity_bytes) {
  entity_buf_.reserve(max_entity_bytes_);
}

void GtfsRtStream::setRouteFilter(std::vector<std::string> routes) {
  route_filter_ = std::move(routes);
}

void GtfsRtStream::setStopFilter(std::vector<std::string> stops) {
  stop_filter_ = std::move(stops);
}

void GtfsRtStream::onUpdate(std::function<void(const StopTimeUpdate&)> cb) {
  on_update_ = std::move(cb);
}

bool GtfsRtStream::routeAllowed(const std::string& route_id) const {
  if (route_filter_.empty()) return true;
  return std::find(route_filter_.begin(), route_filter_.end(), route_id) != route_filter_.end();
}

bool GtfsRtStream::stopAllowed(const std::string& stop_id) const {
  if (stop_filter_.empty()) return true;
  return std::find(stop_filter_.begin(), stop_filter_.end(), stop_id) != stop_filter_.end();
}

bool GtfsRtStream::push(const uint8_t* data, size_t len) {
  if (error_) return false;
  for (size_t i = 0; i < len && !error_; ++i) {
    feedByte(data[i]);
  }
  return !error_;
}

void GtfsRtStream::finish() {
  // No framing terminator exists in protobuf; a mid-entity/mid-header state here means the
  // body was truncated. Leave counters as they are - the caller already knows the transfer
  // was incomplete because push() will have returned before the expected content-length.
}

void GtfsRtStream::feedByte(uint8_t b) {
  switch (state_) {
    case State::kTag: {
      varint_value_ |= static_cast<uint64_t>(b & 0x7f) << varint_shift_;
      if (b & 0x80) {
        varint_shift_ += 7;
        if (varint_shift_ >= 70) { error_ = true; }
        return;
      }
      uint64_t tag = varint_value_;
      varint_value_ = 0;
      varint_shift_ = 0;
      cur_field_ = static_cast<uint32_t>(tag >> 3);
      cur_wiretype_ = static_cast<uint8_t>(tag & 7);
      switch (cur_wiretype_) {
        case 2:
          state_ = State::kLen;
          break;
        case 0:
          state_ = State::kSkipVarint;
          break;
        case 1:
          skip_remaining_ = 8;
          state_ = State::kSkipFixed;
          break;
        case 5:
          skip_remaining_ = 4;
          state_ = State::kSkipFixed;
          break;
        default:
          error_ = true;  // wiretypes 3/4 (groups) unsupported at the top level
          break;
      }
      return;
    }

    case State::kLen: {
      varint_value_ |= static_cast<uint64_t>(b & 0x7f) << varint_shift_;
      if (b & 0x80) {
        varint_shift_ += 7;
        if (varint_shift_ >= 70) { error_ = true; }
        return;
      }
      uint64_t length = varint_value_;
      varint_value_ = 0;
      varint_shift_ = 0;

      if (cur_field_ == 1) {  // FeedHeader
        header_fill_ = 0;
        if (length == 0) {
          decodeHeader(nullptr, 0);
          state_ = State::kTag;
        } else if (length > kHeaderBufCap) {
          // Header bigger than expected (should never happen for a real FeedHeader): skip it
          // rather than risk an unbounded allocation. headerTimestamp() keeps its prior value.
          skip_remaining_ = length;
          state_ = State::kSkipFixed;
        } else {
          header_target_ = static_cast<size_t>(length);
          state_ = State::kHeaderBody;
        }
      } else if (cur_field_ == 2) {  // FeedEntity
        entities_seen_++;
        if (length > max_entity_bytes_) {
          entities_skipped_too_large_++;
          skip_remaining_ = length;
          state_ = State::kSkipFixed;
        } else if (length == 0) {
          decodeEntity(nullptr, 0);
          state_ = State::kTag;
        } else {
          entity_buf_.clear();
          entity_target_ = static_cast<size_t>(length);
          state_ = State::kEntityBody;
        }
      } else {
        // Unknown top-level field with a length-delimited payload: skip it.
        if (length == 0) {
          state_ = State::kTag;
        } else {
          skip_remaining_ = length;
          state_ = State::kSkipFixed;
        }
      }
      return;
    }

    case State::kHeaderBody:
      header_buf_[header_fill_++] = b;
      if (header_fill_ == header_target_) {
        decodeHeader(header_buf_, header_target_);
        state_ = State::kTag;
      }
      return;

    case State::kEntityBody:
      entity_buf_.push_back(b);
      if (entity_buf_.size() == entity_target_) {
        decodeEntity(entity_buf_.data(), entity_buf_.size());
        state_ = State::kTag;
      }
      return;

    case State::kSkipVarint:
      if (!(b & 0x80)) state_ = State::kTag;
      return;

    case State::kSkipFixed:
      if (--skip_remaining_ == 0) state_ = State::kTag;
      return;
  }
}

void GtfsRtStream::decodeHeader(const uint8_t* buf, size_t len) {
  size_t off = 0;
  FieldRef f;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 3 && f.wiretype == 0) {
      header_timestamp_ = static_cast<int64_t>(f.varint);
    }
    // field 1 (version string) and 2 (incrementality) are not needed.
  }
}

void GtfsRtStream::decodeEntity(const uint8_t* buf, size_t len) {
  size_t off = 0;
  FieldRef f;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 3 && f.wiretype == 2) {  // trip_update
      decodeTripUpdate(f.data, f.len);
    }
    // field 1 (id) is not needed; vehicle-position/alert entities (fields 2/4) are ignored.
  }
}

void GtfsRtStream::decodeTripUpdate(const uint8_t* buf, size_t len) {
  std::string trip_id, route_id, vehicle_id;
  int direction_id = -1;

  // Pass 1: trip descriptor + vehicle descriptor (order-independent in the wire format).
  {
    size_t off = 0;
    FieldRef f;
    while (readNextField(buf, len, off, f)) {
      if (f.field == 1 && f.wiretype == 2) {
        decodeTripDescriptor(f.data, f.len, &trip_id, &route_id, &direction_id);
      } else if (f.field == 3 && f.wiretype == 2) {
        decodeVehicleDescriptor(f.data, f.len, &vehicle_id);
      }
      // field 4 (timestamp) and 5 (delay) are trip-level and not surfaced (see header comment).
    }
  }

  if (!routeAllowed(route_id)) return;  // early exit: skip the whole trip's stop_time_updates

  // Pass 2: stop_time_update entries, now that trip/vehicle context is known.
  {
    size_t off = 0;
    FieldRef f;
    while (readNextField(buf, len, off, f)) {
      if (f.field == 2 && f.wiretype == 2) {
        decodeStopTimeUpdate(f.data, f.len, trip_id, route_id, vehicle_id, direction_id);
      }
    }
  }
}

void GtfsRtStream::decodeTripDescriptor(const uint8_t* buf, size_t len, std::string* trip_id,
                                         std::string* route_id, int* direction_id) {
  size_t off = 0;
  FieldRef f;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 1 && f.wiretype == 2) {
      trip_id->assign(reinterpret_cast<const char*>(f.data), f.len);
    } else if (f.field == 5 && f.wiretype == 2) {
      route_id->assign(reinterpret_cast<const char*>(f.data), f.len);
    } else if (f.field == 6 && f.wiretype == 0) {
      *direction_id = static_cast<int>(f.varint);
    }
    // field 4 (schedule_relationship) is parsed-and-skipped; see the header comment for why.
  }
}

void GtfsRtStream::decodeVehicleDescriptor(const uint8_t* buf, size_t len,
                                            std::string* vehicle_id) {
  size_t off = 0;
  FieldRef f;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 1 && f.wiretype == 2) {
      vehicle_id->assign(reinterpret_cast<const char*>(f.data), f.len);
    }
    // field 2 (label) is not needed.
  }
}

void GtfsRtStream::decodeStopTimeUpdate(const uint8_t* buf, size_t len,
                                         const std::string& trip_id, const std::string& route_id,
                                         const std::string& vehicle_id, int direction_id) {
  uint32_t stop_sequence = 0;
  std::string stop_id;
  int64_t arrival_time = 0, departure_time = 0;
  int32_t arrival_delay = 0, departure_delay = 0;
  bool has_arrival = false, has_departure = false;
  uint8_t schedule_relationship = 0;

  size_t off = 0;
  FieldRef f;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 1 && f.wiretype == 0) {
      stop_sequence = static_cast<uint32_t>(f.varint);
    } else if (f.field == 4 && f.wiretype == 2) {
      stop_id.assign(reinterpret_cast<const char*>(f.data), f.len);
    } else if (f.field == 2 && f.wiretype == 2) {
      decodeStopTimeEvent(f.data, f.len, &arrival_delay, &arrival_time, &has_arrival);
    } else if (f.field == 3 && f.wiretype == 2) {
      decodeStopTimeEvent(f.data, f.len, &departure_delay, &departure_time, &has_departure);
    } else if (f.field == 5 && f.wiretype == 0) {
      schedule_relationship = static_cast<uint8_t>(f.varint);
    }
  }

  if (!stopAllowed(stop_id)) return;

  StopTimeUpdate out;
  out.trip_id = trip_id;
  out.route_id = route_id;
  out.vehicle_id = vehicle_id;
  out.stop_id = stop_id;
  out.direction_id = direction_id;
  out.stop_sequence = stop_sequence;
  out.arrival_time = arrival_time;
  out.departure_time = departure_time;
  out.arrival_delay = arrival_delay;
  out.departure_delay = departure_delay;
  out.schedule_relationship = schedule_relationship;
  out.has_arrival_time = has_arrival;
  out.has_departure_time = has_departure;

  if (on_update_) on_update_(out);
}

void GtfsRtStream::decodeStopTimeEvent(const uint8_t* buf, size_t len, int32_t* delay,
                                        int64_t* time, bool* has_time) {
  size_t off = 0;
  FieldRef f;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 1 && f.wiretype == 0) {
      *delay = toInt32(f.varint);
    } else if (f.field == 2 && f.wiretype == 0) {
      *time = static_cast<int64_t>(f.varint);
      *has_time = true;
    }
    // field 3 (uncertainty) is not needed.
  }
}

uint32_t GtfsRtStream::entitiesSeen() const { return entities_seen_; }
uint32_t GtfsRtStream::entitiesSkippedTooLarge() const { return entities_skipped_too_large_; }
int64_t GtfsRtStream::headerTimestamp() const { return header_timestamp_; }

}  // namespace transit
