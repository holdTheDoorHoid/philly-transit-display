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
  own_entity_buf_.reserve(max_entity_bytes_);
}

void GtfsRtStream::setEntityBuffer(std::vector<uint8_t>* borrowed) {
  borrowed_entity_buf_ = borrowed;
  if (borrowed != nullptr) {
    // Give back whatever this object was holding: the whole point of the borrow is that one
    // reservation serves several stages, so keeping a second one would defeat it.
    std::vector<uint8_t>().swap(own_entity_buf_);
    if (borrowed->capacity() < max_entity_bytes_) borrowed->reserve(max_entity_bytes_);
  }
}

void GtfsRtStream::reset(size_t max_entity_bytes) {
  max_entity_bytes_ = max_entity_bytes;
  std::vector<uint8_t>& buf = entityBuf();
  buf.clear();  // keeps the capacity: that is the whole point of reusing the object
  if (buf.capacity() < max_entity_bytes_) buf.reserve(max_entity_bytes_);
  entity_target_ = 0;

  route_filter_.clear();
  stop_filter_.clear();
  on_update_ = nullptr;

  // Retention off until the caller asks for it again, exactly as after construction.
  // retainUpdates() clear()s and reserve()s, so the block below it is kept too.
  retained_.clear();
  max_retained_ = 0;
  max_per_stop_route_ = 0;

  state_ = State::kTag;
  error_ = false;
  varint_value_ = 0;
  varint_shift_ = 0;
  cur_field_ = 0;
  cur_wiretype_ = 0;
  skip_remaining_ = 0;
  header_fill_ = 0;
  header_target_ = 0;

  entities_seen_ = 0;
  entities_skipped_too_large_ = 0;
  entities_matched_ = 0;
  entities_malformed_ = 0;
  updates_matched_ = 0;
  updates_dropped_by_cap_ = 0;
  identifiers_truncated_ = 0;
  header_timestamp_ = 0;
}

void GtfsRtStream::setRouteFilter(std::vector<std::string> routes) {
  route_filter_ = std::move(routes);
}

void GtfsRtStream::retainUpdates(size_t max_total, size_t max_per_stop_route) {
  max_retained_ = max_total;
  max_per_stop_route_ = max_per_stop_route;
  retained_.clear();
  retained_.reserve(max_total);  // the only allocation this buffer ever makes
}

const std::vector<StopTimeUpdate>& GtfsRtStream::retained() const { return retained_; }

// Time to rank a retained update by, for the "keep the nearest" eviction rule. An update with no
// time of its own (SKIPPED/NO_DATA - see StopRel) is ranked at the feed's own timestamp rather
// than at 0: it is a statement about imminent service, so it should not be evicted in favour of
// a prediction two hours out, but it should not outrank everything either.
static int64_t rankOf(const StopTimeUpdate& u) {
  int64_t t = u.predictedTime();
  return t != 0 ? t : u.feed_timestamp;
}

void GtfsRtStream::retain(StopTimeUpdate&& u) {
  if (max_retained_ == 0) return;

  // Per-(stop, route) cap first: one busy route must not crowd every other configured stop out
  // of the global cap.
  size_t same_key = 0, worst_key_idx = retained_.size();
  int64_t worst_key_rank = 0;
  for (size_t i = 0; i < retained_.size(); ++i) {
    if (retained_[i].stop_id != u.stop_id || retained_[i].route_id != u.route_id) continue;
    ++same_key;
    int64_t r = rankOf(retained_[i]);
    if (worst_key_idx == retained_.size() || r > worst_key_rank) {
      worst_key_idx = i;
      worst_key_rank = r;
    }
  }
  if (same_key >= max_per_stop_route_) {
    ++updates_dropped_by_cap_;
    if (worst_key_idx < retained_.size() && rankOf(u) < worst_key_rank) {
      retained_[worst_key_idx] = std::move(u);
    }
    return;
  }

  if (retained_.size() < max_retained_) {
    retained_.push_back(std::move(u));
    return;
  }

  // Global cap: evict the farthest-future retained update, if this one is nearer.
  size_t worst = 0;
  int64_t worst_rank = rankOf(retained_[0]);
  for (size_t i = 1; i < retained_.size(); ++i) {
    int64_t r = rankOf(retained_[i]);
    if (r > worst_rank) {
      worst = i;
      worst_rank = r;
    }
  }
  ++updates_dropped_by_cap_;
  if (rankOf(u) < worst_rank) retained_[worst] = std::move(u);
}

void GtfsRtStream::assignCapped(std::string* dst, const uint8_t* data, size_t len) {
  if (len > kMaxIdentifierChars) {
    len = kMaxIdentifierChars;
    ++identifiers_truncated_;
  }
  dst->assign(reinterpret_cast<const char*>(data), len);
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

FeedStatus GtfsRtStream::finish() { return feedStatus(); }

FeedStatus GtfsRtStream::feedStatus() const {
  // No framing terminator exists in protobuf, so "complete" is precisely "the last byte landed on
  // a top-level field boundary": state kTag with no partially-accumulated varint. Anything else
  // means the body stopped arriving mid-field. This is the whole reason finish() reports rather
  // than returning void - a connection cut halfway through the 150 KB feed is otherwise
  // indistinguishable from a legitimately short one, and every stop it should have carried comes
  // back silently empty (F13).
  if (error_) return FeedStatus::Malformed;
  if (state_ != State::kTag || varint_shift_ != 0 || varint_value_ != 0) {
    return FeedStatus::Truncated;
  }
  return FeedStatus::Complete;
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
          entityBuf().clear();
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
      entityBuf().push_back(b);
      if (entityBuf().size() == entity_target_) {
        decodeEntity(entityBuf().data(), entityBuf().size());
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
  bool matched = false;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 3 && f.wiretype == 2) {  // trip_update
      decodeTripUpdate(f.data, f.len, &matched);
    }
    // field 1 (id) is not needed; vehicle-position/alert entities (fields 2/4) are ignored.
  }
  // readNextField() stops both at the clean end of the message and on bad framing; `off` short of
  // `len` distinguishes them. A malformed entity is skipped (the outer stream already knows its
  // exact byte length, so the next entity still parses) but counted, so a caller can tell
  // "nothing matched" apart from "the feed is partly corrupt" - see entitiesMalformed().
  if (off < len) ++entities_malformed_;
  if (matched) ++entities_matched_;
}

void GtfsRtStream::decodeTripUpdate(const uint8_t* buf, size_t len, bool* matched) {
  std::string trip_id, route_id, vehicle_id;
  int direction_id = -1;
  uint8_t trip_relationship = 0;

  // Pass 1: trip descriptor + vehicle descriptor (order-independent in the wire format).
  {
    size_t off = 0;
    FieldRef f;
    while (readNextField(buf, len, off, f)) {
      if (f.field == 1 && f.wiretype == 2) {
        decodeTripDescriptor(f.data, f.len, &trip_id, &route_id, &direction_id,
                              &trip_relationship);
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
        decodeStopTimeUpdate(f.data, f.len, trip_id, route_id, vehicle_id, direction_id,
                              trip_relationship, matched);
      }
    }
  }
}

void GtfsRtStream::decodeTripDescriptor(const uint8_t* buf, size_t len, std::string* trip_id,
                                         std::string* route_id, int* direction_id,
                                         uint8_t* trip_relationship) {
  size_t off = 0;
  FieldRef f;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 1 && f.wiretype == 2) {
      assignCapped(trip_id, f.data, f.len);
    } else if (f.field == 5 && f.wiretype == 2) {
      assignCapped(route_id, f.data, f.len);
    } else if (f.field == 6 && f.wiretype == 0) {
      *direction_id = static_cast<int>(f.varint);
    } else if (f.field == 4 && f.wiretype == 0) {
      // Trip-level schedule_relationship (TripRel). Surfaced - but into its OWN field, never
      // merged with the stop-level one: a CANCELED trip used to be parsed and thrown away here,
      // which is how a cancelled trip kept being displayed as an ordinary scheduled arrival.
      *trip_relationship = f.varint > 255 ? 0 : static_cast<uint8_t>(f.varint);
    }
  }
}

void GtfsRtStream::decodeVehicleDescriptor(const uint8_t* buf, size_t len,
                                            std::string* vehicle_id) {
  size_t off = 0;
  FieldRef f;
  while (readNextField(buf, len, off, f)) {
    if (f.field == 1 && f.wiretype == 2) {
      assignCapped(vehicle_id, f.data, f.len);
    }
    // field 2 (label) is not needed.
  }
}

void GtfsRtStream::decodeStopTimeUpdate(const uint8_t* buf, size_t len,
                                         const std::string& trip_id, const std::string& route_id,
                                         const std::string& vehicle_id, int direction_id,
                                         uint8_t trip_relationship, bool* matched) {
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
      assignCapped(&stop_id, f.data, f.len);
    } else if (f.field == 2 && f.wiretype == 2) {
      decodeStopTimeEvent(f.data, f.len, &arrival_delay, &arrival_time, &has_arrival);
    } else if (f.field == 3 && f.wiretype == 2) {
      decodeStopTimeEvent(f.data, f.len, &departure_delay, &departure_time, &has_departure);
    } else if (f.field == 5 && f.wiretype == 0) {
      schedule_relationship = f.varint > 255 ? 0 : static_cast<uint8_t>(f.varint);
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
  out.trip_schedule_relationship = trip_relationship;
  // Carried per-update, not just queryable from the stream, so a merge that is handed a plain
  // vector<StopTimeUpdate> (as mergeStop is) can still tell how old the predictions are.
  out.feed_timestamp = header_timestamp_;
  out.has_arrival_time = has_arrival;
  out.has_departure_time = has_departure;

  ++updates_matched_;
  if (matched) *matched = true;
  if (on_update_) on_update_(out);
  retain(std::move(out));
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
uint32_t GtfsRtStream::entitiesMatched() const { return entities_matched_; }
uint32_t GtfsRtStream::entitiesMalformed() const { return entities_malformed_; }
uint32_t GtfsRtStream::updatesMatched() const { return updates_matched_; }
uint32_t GtfsRtStream::updatesDroppedByCap() const { return updates_dropped_by_cap_; }
uint32_t GtfsRtStream::identifiersTruncated() const { return identifiers_truncated_; }
int64_t GtfsRtStream::headerTimestamp() const { return header_timestamp_; }

}  // namespace transit
