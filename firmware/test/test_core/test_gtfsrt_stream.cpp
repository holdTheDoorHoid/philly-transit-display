#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "fixture_path.h"
#include "transit_core/gtfsrt_stream.h"

using transit::GtfsRtStream;
using transit::StopTimeUpdate;

namespace {

// --- tiny protobuf encoder, used only to build a synthetic oversized-entity case the real
// fixture doesn't naturally contain (its largest real entity is ~2.3 KB, under the 4096 cap) ---

std::vector<uint8_t> encodeVarint(uint64_t v) {
  std::vector<uint8_t> out;
  do {
    uint8_t b = v & 0x7f;
    v >>= 7;
    if (v) b |= 0x80;
    out.push_back(b);
  } while (v);
  return out;
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

std::vector<uint8_t> tagBytes(int field, int wiretype) {
  return encodeVarint(static_cast<uint64_t>((field << 3) | wiretype));
}

std::vector<uint8_t> varintField(int field, uint64_t v) {
  std::vector<uint8_t> out = tagBytes(field, 0);
  append(out, encodeVarint(v));
  return out;
}

std::vector<uint8_t> lenDelimField(int field, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out = tagBytes(field, 2);
  append(out, encodeVarint(payload.size()));
  append(out, payload);
  return out;
}

std::vector<uint8_t> stringField(int field, const std::string& s) {
  return lenDelimField(field, std::vector<uint8_t>(s.begin(), s.end()));
}

std::vector<uint8_t> buildTripDescriptor(const std::string& trip_id, const std::string& route_id,
                                          int direction_id) {
  std::vector<uint8_t> td;
  append(td, stringField(1, trip_id));
  append(td, stringField(5, route_id));
  append(td, varintField(6, static_cast<uint64_t>(direction_id)));
  return td;
}

std::vector<uint8_t> buildStopTimeUpdate(uint32_t seq, const std::string& stop_id,
                                          int64_t arrival_time) {
  std::vector<uint8_t> arr_event = varintField(2, static_cast<uint64_t>(arrival_time));
  std::vector<uint8_t> stu;
  append(stu, varintField(1, seq));
  append(stu, lenDelimField(2, arr_event));
  append(stu, stringField(4, stop_id));
  return stu;
}

std::vector<uint8_t> buildEntity(const std::string& id, const std::string& trip_id,
                                  const std::string& route_id, int direction_id,
                                  const std::string& vehicle_id, uint32_t seq,
                                  const std::string& stop_id, int64_t arrival_time) {
  std::vector<uint8_t> tu;
  append(tu, lenDelimField(1, buildTripDescriptor(trip_id, route_id, direction_id)));
  append(tu, lenDelimField(2, buildStopTimeUpdate(seq, stop_id, arrival_time)));
  append(tu, lenDelimField(3, stringField(1, vehicle_id)));

  std::vector<uint8_t> entity;
  append(entity, stringField(1, id));
  append(entity, lenDelimField(3, tu));
  return entity;
}

std::vector<uint8_t> wrapTopLevelEntity(const std::vector<uint8_t>& entity_bytes) {
  return lenDelimField(2, entity_bytes);
}

std::vector<uint8_t> buildHeader(int64_t timestamp) {
  std::vector<uint8_t> hdr;
  append(hdr, stringField(1, "2.0"));
  append(hdr, varintField(3, static_cast<uint64_t>(timestamp)));
  return hdr;
}

bool sameUpdate(const StopTimeUpdate& a, const StopTimeUpdate& b) {
  return a.trip_id == b.trip_id && a.route_id == b.route_id && a.vehicle_id == b.vehicle_id &&
         a.stop_id == b.stop_id && a.direction_id == b.direction_id &&
         a.stop_sequence == b.stop_sequence && a.arrival_time == b.arrival_time &&
         a.departure_time == b.departure_time && a.arrival_delay == b.arrival_delay &&
         a.departure_delay == b.departure_delay &&
         a.schedule_relationship == b.schedule_relationship &&
         a.has_arrival_time == b.has_arrival_time && a.has_departure_time == b.has_departure_time;
}

std::vector<StopTimeUpdate> decodeFixtureInChunks(const std::vector<uint8_t>& body,
                                                   size_t chunk_size, int64_t* header_ts_out) {
  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"21332", "21297"});
  std::vector<StopTimeUpdate> updates;
  stream.onUpdate([&](const StopTimeUpdate& u) { updates.push_back(u); });

  size_t i = 0;
  while (i < body.size()) {
    size_t n = std::min(chunk_size, body.size() - i);
    TEST_ASSERT_TRUE(stream.push(body.data() + i, n));
    i += n;
  }
  stream.finish();
  if (header_ts_out) *header_ts_out = stream.headerTimestamp();
  return updates;
}

}  // namespace

void test_gtfsrt_known_values_and_header_timestamp() {
  std::vector<uint8_t> body = transit_test::readFixture("septa_bus_tripupdates.pb");
  TEST_ASSERT_TRUE(body.size() > 0);

  int64_t header_ts = 0;
  std::vector<StopTimeUpdate> updates = decodeFixtureInChunks(body, body.size(), &header_ts);

  TEST_ASSERT_EQUAL_INT64(1789352333, header_ts);

  const StopTimeUpdate* found = nullptr;
  for (const auto& u : updates) {
    if (u.trip_id == "3667" && u.stop_id == "21332") {
      found = &u;
      break;
    }
  }
  TEST_ASSERT_NOT_NULL(found);
  TEST_ASSERT_EQUAL_STRING("17", found->route_id.c_str());
  TEST_ASSERT_EQUAL_STRING("7477", found->vehicle_id.c_str());
  TEST_ASSERT_EQUAL_UINT32(35, found->stop_sequence);
  TEST_ASSERT_EQUAL_INT64(1789353562, found->arrival_time);
  TEST_ASSERT_TRUE(found->has_arrival_time);
  TEST_ASSERT_EQUAL_INT(1, found->direction_id);
}

void test_gtfsrt_identical_across_chunk_sizes() {
  std::vector<uint8_t> body = transit_test::readFixture("septa_bus_tripupdates.pb");
  TEST_ASSERT_TRUE(body.size() > 0);

  const size_t chunk_sizes[] = {1, 7, 100, 1000, 4096, body.size()};
  std::vector<StopTimeUpdate> baseline;
  for (size_t ci = 0; ci < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); ++ci) {
    int64_t ts = 0;
    std::vector<StopTimeUpdate> updates = decodeFixtureInChunks(body, chunk_sizes[ci], &ts);
    TEST_ASSERT_EQUAL_INT64(1789352333, ts);
    if (ci == 0) {
      baseline = updates;
      TEST_ASSERT_TRUE(baseline.size() > 0);
      continue;
    }
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(baseline.size()),
                              static_cast<uint32_t>(updates.size()));
    for (size_t i = 0; i < baseline.size(); ++i) {
      TEST_ASSERT_TRUE(sameUpdate(baseline[i], updates[i]));
    }
  }
}

void test_gtfsrt_route_and_stop_filters() {
  std::vector<uint8_t> body = transit_test::readFixture("septa_bus_tripupdates.pb");
  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"21332", "21297"});
  std::vector<StopTimeUpdate> updates;
  stream.onUpdate([&](const StopTimeUpdate& u) { updates.push_back(u); });
  TEST_ASSERT_TRUE(stream.push(body.data(), body.size()));
  stream.finish();

  TEST_ASSERT_TRUE(updates.size() > 0);
  for (const auto& u : updates) {
    TEST_ASSERT_EQUAL_STRING("17", u.route_id.c_str());
    TEST_ASSERT_TRUE(u.stop_id == "21332" || u.stop_id == "21297");
  }
  TEST_ASSERT_TRUE(stream.entitiesSeen() > 0);
}

void test_gtfsrt_oversized_entity_skipped_without_corrupting_stream() {
  std::vector<uint8_t> msg;
  append(msg, lenDelimField(1, buildHeader(1000)));
  append(msg, wrapTopLevelEntity(buildEntity("A", "A1", "17", 0, "vA", 1, "S1", 111)));

  // A synthetic entity far larger than the (default 4096-byte) cap. Its internal bytes are not
  // valid protobuf - that's the point: push() must skip it purely by length, without attempting
  // to interpret its contents, and without disturbing the next entity's parse.
  std::vector<uint8_t> huge(6000, 0xAB);
  append(msg, lenDelimField(2, huge));

  append(msg, wrapTopLevelEntity(buildEntity("B", "B1", "17", 0, "vB", 1, "S1", 222)));

  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"S1"});
  std::vector<StopTimeUpdate> updates;
  stream.onUpdate([&](const StopTimeUpdate& u) { updates.push_back(u); });

  TEST_ASSERT_TRUE(stream.push(msg.data(), msg.size()));
  stream.finish();

  TEST_ASSERT_EQUAL_UINT32(3, stream.entitiesSeen());
  TEST_ASSERT_EQUAL_UINT32(1, stream.entitiesSkippedTooLarge());
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(updates.size()));
  TEST_ASSERT_EQUAL_STRING("A1", updates[0].trip_id.c_str());
  TEST_ASSERT_EQUAL_INT64(111, updates[0].arrival_time);
  TEST_ASSERT_EQUAL_STRING("B1", updates[1].trip_id.c_str());
  TEST_ASSERT_EQUAL_INT64(222, updates[1].arrival_time);
  TEST_ASSERT_EQUAL_INT64(1000, stream.headerTimestamp());
}

void test_gtfsrt_oversized_entity_respects_custom_cap() {
  // Same shape as above but with a small explicit cap, so the "oversized" entity here is only
  // slightly bigger than a normal one - exercises the boundary rather than a huge multiple.
  GtfsRtStream stream(32);  // tiny cap: even buildEntity(...)'s normal output likely exceeds it
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"S1"});
  std::vector<StopTimeUpdate> updates;
  stream.onUpdate([&](const StopTimeUpdate& u) { updates.push_back(u); });

  std::vector<uint8_t> entity = buildEntity("A", "A1", "17", 0, "vA", 1, "S1", 111);
  TEST_ASSERT_TRUE(entity.size() > 32);  // sanity: this test only makes sense if it overflows
  std::vector<uint8_t> msg = wrapTopLevelEntity(entity);

  TEST_ASSERT_TRUE(stream.push(msg.data(), msg.size()));
  stream.finish();

  TEST_ASSERT_EQUAL_UINT32(1, stream.entitiesSeen());
  TEST_ASSERT_EQUAL_UINT32(1, stream.entitiesSkippedTooLarge());
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(updates.size()));
}

void test_gtfsrt_empty_stream_has_zero_counts() {
  GtfsRtStream stream;
  TEST_ASSERT_EQUAL_UINT32(0, stream.entitiesSeen());
  TEST_ASSERT_EQUAL_UINT32(0, stream.entitiesSkippedTooLarge());
  TEST_ASSERT_EQUAL_INT64(0, stream.headerTimestamp());
  stream.finish();
}

// =============================================================================================
// Regression tests for the 2026-09-15 adversarial review (F06 retention bounds, F13 truncation
// detection, F15 feed age, F16 trip-level CANCELED).
// =============================================================================================

namespace {

// buildEntity() with the extra fields these tests need: a trip-level schedule_relationship, and
// a stop_time_update that carries no time at all (what a real SKIPPED update usually looks like).
std::vector<uint8_t> buildEntityEx(const std::string& id, const std::string& trip_id,
                                    const std::string& route_id, const std::string& stop_id,
                                    int64_t arrival_time, int trip_relationship,
                                    int stop_relationship) {
  std::vector<uint8_t> td;
  append(td, stringField(1, trip_id));
  if (trip_relationship >= 0) append(td, varintField(4, static_cast<uint64_t>(trip_relationship)));
  append(td, stringField(5, route_id));
  append(td, varintField(6, 0));

  std::vector<uint8_t> stu;
  append(stu, varintField(1, 1));
  if (arrival_time != 0) {
    append(stu, lenDelimField(2, varintField(2, static_cast<uint64_t>(arrival_time))));
  }
  append(stu, stringField(4, stop_id));
  if (stop_relationship >= 0) append(stu, varintField(5, static_cast<uint64_t>(stop_relationship)));

  std::vector<uint8_t> tu;
  append(tu, lenDelimField(1, td));
  append(tu, lenDelimField(2, stu));

  std::vector<uint8_t> entity;
  append(entity, stringField(1, id));
  append(entity, lenDelimField(3, tu));
  return entity;
}

}  // namespace

// --- F06: retained data must be bounded, and bounded in favour of the NEAREST arrivals --------

// The review's case: a feed whose entities all match. Every one of them used to be appended to
// the caller's vector with no limit at all.
void test_gtfsrt_retention_cap_holds_at_the_cap_with_500_matching_entities() {
  const int64_t kBase = 1789352333;
  std::vector<uint8_t> msg;
  append(msg, lenDelimField(1, buildHeader(kBase)));
  // Deliberately farthest-first, so "keep the nearest" has to actually evict rather than just
  // stop accepting: entity i arrives at kBase + (500 - i) * 60.
  for (int i = 0; i < 500; ++i) {
    append(msg, wrapTopLevelEntity(buildEntityEx("e" + std::to_string(i), "t" + std::to_string(i),
                                                  "17", "21332", kBase + (500 - i) * 60, -1, -1)));
  }

  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"21332"});
  stream.retainUpdates();  // defaults: 64 total, 12 per (stop, route)
  TEST_ASSERT_TRUE(stream.push(msg.data(), msg.size()));
  TEST_ASSERT_TRUE(stream.finish() == transit::FeedStatus::Complete);

  TEST_ASSERT_EQUAL_UINT32(500, stream.entitiesSeen());
  TEST_ASSERT_EQUAL_UINT32(500, stream.entitiesMatched());
  TEST_ASSERT_EQUAL_UINT32(500, stream.updatesMatched());
  // All 500 are the same (stop, route), so the per-pair cap is what binds here.
  TEST_ASSERT_EQUAL_UINT32(12, static_cast<uint32_t>(stream.retained().size()));
  TEST_ASSERT_EQUAL_UINT32(488, stream.updatesDroppedByCap());

  // What survived is the 12 nearest, i.e. the last 12 entities in wire order.
  int64_t worst = 0;
  for (const auto& u : stream.retained()) {
    TEST_ASSERT_TRUE(u.arrival_time >= kBase + 60);
    TEST_ASSERT_TRUE(u.arrival_time <= kBase + 12 * 60);
    if (u.arrival_time > worst) worst = u.arrival_time;
  }
  TEST_ASSERT_EQUAL_INT64(kBase + 12 * 60, worst);
}

// With many distinct (stop, route) pairs it is the global cap that binds, and it binds hard.
void test_gtfsrt_retention_global_cap_across_many_stops() {
  const int64_t kBase = 1789352333;
  std::vector<uint8_t> msg;
  append(msg, lenDelimField(1, buildHeader(kBase)));
  for (int i = 0; i < 500; ++i) {
    append(msg, wrapTopLevelEntity(buildEntityEx("e" + std::to_string(i), "t" + std::to_string(i),
                                                  "17", "stop" + std::to_string(i),
                                                  kBase + (500 - i) * 60, -1, -1)));
  }

  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.retainUpdates();
  TEST_ASSERT_TRUE(stream.push(msg.data(), msg.size()));
  stream.finish();

  TEST_ASSERT_EQUAL_UINT32(64, static_cast<uint32_t>(stream.retained().size()));
  TEST_ASSERT_EQUAL_UINT32(436, stream.updatesDroppedByCap());
  for (const auto& u : stream.retained()) {
    TEST_ASSERT_TRUE(u.arrival_time <= kBase + 64 * 60);  // the nearest 64 kept
  }
}

// Identifiers are bounded too: the wire format allows any length, and a retained update holds
// four of them.
void test_gtfsrt_long_identifiers_are_truncated() {
  std::string long_trip(500, 'T');
  std::vector<uint8_t> msg;
  append(msg, lenDelimField(1, buildHeader(1000)));
  append(msg, wrapTopLevelEntity(buildEntityEx("e", long_trip, "17", "S1", 1111, -1, -1)));

  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"S1"});
  std::vector<StopTimeUpdate> updates;
  stream.onUpdate([&](const StopTimeUpdate& u) { updates.push_back(u); });
  TEST_ASSERT_TRUE(stream.push(msg.data(), msg.size()));
  stream.finish();

  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(updates.size()));
  TEST_ASSERT_EQUAL_UINT32(GtfsRtStream::kMaxIdentifierChars,
                            static_cast<uint32_t>(updates[0].trip_id.size()));
  TEST_ASSERT_TRUE(stream.identifiersTruncated() > 0);
}

// --- F13: finish() has to say whether the body actually finished -------------------------------

void test_gtfsrt_complete_body_reports_complete() {
  std::vector<uint8_t> body = transit_test::readFixture("septa_bus_tripupdates.pb");
  GtfsRtStream stream;
  TEST_ASSERT_TRUE(stream.push(body.data(), body.size()));
  TEST_ASSERT_TRUE(stream.finish() == transit::FeedStatus::Complete);
}

void test_gtfsrt_truncated_body_reports_truncated() {
  std::vector<uint8_t> body = transit_test::readFixture("septa_bus_tripupdates.pb");
  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"21332"});
  // One byte short of the end, which lands inside the final entity's payload: a dropped
  // connection, not a short feed. (Any mid-entity cut does; this one is deterministic.)
  TEST_ASSERT_TRUE(stream.push(body.data(), body.size() - 1));
  TEST_ASSERT_TRUE(stream.finish() == transit::FeedStatus::Truncated);
  TEST_ASSERT_TRUE(stream.entitiesSeen() > 0);  // it decoded what it got - but said so
}

void test_gtfsrt_empty_body_is_complete_not_truncated() {
  GtfsRtStream stream;
  TEST_ASSERT_TRUE(stream.finish() == transit::FeedStatus::Complete);
}

// A nested message that does not decode is skipped and counted; the entities after it still
// parse, so the feed as a whole is not condemned.
void test_gtfsrt_malformed_entity_is_counted_not_fatal() {
  std::vector<uint8_t> msg;
  append(msg, lenDelimField(1, buildHeader(1000)));
  // An "entity" whose declared inner length runs past its own bytes.
  std::vector<uint8_t> bad;
  append(bad, tagBytes(3, 2));
  append(bad, encodeVarint(200));  // claims 200 bytes of trip_update...
  bad.push_back(0x01);             // ...and provides one
  append(msg, lenDelimField(2, bad));
  append(msg, wrapTopLevelEntity(buildEntityEx("B", "B1", "17", "S1", 222, -1, -1)));

  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"S1"});
  std::vector<StopTimeUpdate> updates;
  stream.onUpdate([&](const StopTimeUpdate& u) { updates.push_back(u); });
  TEST_ASSERT_TRUE(stream.push(msg.data(), msg.size()));
  TEST_ASSERT_TRUE(stream.finish() == transit::FeedStatus::Complete);

  TEST_ASSERT_EQUAL_UINT32(1, stream.entitiesMalformed());
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(updates.size()));
  TEST_ASSERT_EQUAL_STRING("B1", updates[0].trip_id.c_str());
}

// --- F15/F16: the trip-level relationship and the feed timestamp reach the caller --------------

void test_gtfsrt_trip_level_canceled_is_surfaced() {
  std::vector<uint8_t> msg;
  append(msg, lenDelimField(1, buildHeader(1789352333)));
  append(msg, wrapTopLevelEntity(buildEntityEx("A", "A1", "17", "S1", 1789353000,
                                                3 /* CANCELED */, -1)));
  append(msg, wrapTopLevelEntity(buildEntityEx("B", "B1", "17", "S1", 1789353100, -1,
                                                1 /* stop-level SKIPPED */)));

  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"S1"});
  std::vector<StopTimeUpdate> updates;
  stream.onUpdate([&](const StopTimeUpdate& u) { updates.push_back(u); });
  TEST_ASSERT_TRUE(stream.push(msg.data(), msg.size()));
  stream.finish();

  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(updates.size()));
  TEST_ASSERT_TRUE(updates[0].tripCanceled());
  TEST_ASSERT_FALSE(updates[0].stopSkipped());  // the two enums stay separate
  TEST_ASSERT_FALSE(updates[1].tripCanceled());
  TEST_ASSERT_TRUE(updates[1].stopSkipped());
  // Every update carries the feed's own header timestamp, so a merge that only sees the updates
  // can still judge how old the predictions are.
  TEST_ASSERT_EQUAL_INT64(1789352333, updates[0].feed_timestamp);
  TEST_ASSERT_EQUAL_INT64(1789352333, updates[1].feed_timestamp);
}

void test_gtfsrt_fixture_updates_carry_the_header_timestamp() {
  std::vector<uint8_t> body = transit_test::readFixture("septa_bus_tripupdates.pb");
  GtfsRtStream stream;
  stream.setRouteFilter({"17"});
  stream.setStopFilter({"21332"});
  stream.retainUpdates();
  TEST_ASSERT_TRUE(stream.push(body.data(), body.size()));
  stream.finish();
  TEST_ASSERT_TRUE(stream.retained().size() > 0);
  for (const auto& u : stream.retained()) {
    TEST_ASSERT_EQUAL_INT64(1789352333, u.feed_timestamp);
  }
}
