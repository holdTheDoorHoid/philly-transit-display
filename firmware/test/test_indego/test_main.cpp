#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "indego_core/indego.h"

using namespace indego;

namespace {

std::vector<uint8_t> readFixture(const char* name) {
  std::string path = std::string("test/fixtures/") + name;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::string here = __FILE__;  // .../firmware/test/test_indego/test_main.cpp
    here.erase(here.find_last_of("/\\"));
    here.erase(here.find_last_of("/\\"));
    path = here + "/fixtures/" + name;
    f = std::fopen(path.c_str(), "rb");
  }
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "fixture not found");
  std::vector<uint8_t> out;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

// Feeds the whole body to a fresh StatusStream in chunks of `chunk`, then finish()es it.
StatusStream runStream(const std::vector<uint8_t>& body, const std::vector<int>& filter, size_t chunk) {
  StatusStream s;
  s.setStationFilter(filter);
  size_t i = 0;
  while (i < body.size()) {
    size_t n = std::min(chunk, body.size() - i);
    if (!s.push(body.data() + i, n)) break;  // early abort: caller may stop feeding
    i += n;
  }
  s.finish();
  return s;
}

// Standalone (does not use StatusStream) scan of a features[] body for feature byte spans, used
// only to slice out one real feature's exact bytes for the oversize-feature test below.
std::vector<std::pair<size_t, size_t>> findFeatureSpans(const std::string& body) {
  std::vector<std::pair<size_t, size_t>> spans;
  size_t arr = body.find("\"features\":[");
  if (arr == std::string::npos) return spans;
  size_t i = arr + std::strlen("\"features\":[");
  bool in_string = false, escape = false;
  int depth = 0;
  size_t start = 0;
  for (; i < body.size(); ++i) {
    char c = body[i];
    if (depth == 0) {
      if (c == '{') {
        start = i;
        depth = 1;
        in_string = false;
        escape = false;
      } else if (c == ']') {
        break;
      }
      continue;
    }
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
    } else {
      if (c == '"') {
        in_string = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
        if (depth == 0) spans.push_back({start, i + 1});
      }
    }
  }
  return spans;
}

std::string extractFeature(const std::string& body, const std::string& idNeedle) {
  for (auto& sp : findFeatureSpans(body)) {
    std::string feat = body.substr(sp.first, sp.second - sp.first);
    if (feat.find(idNeedle) != std::string::npos) return feat;
  }
  TEST_FAIL_MESSAGE("feature not found in fixture");
  return "";
}

std::string bigFeature(int id) {
  std::string s = "{\"geometry\":{\"coordinates\":[-75.0,39.9],\"type\":\"Point\"},\"properties\":{\"id\":";
  s += std::to_string(id);
  s += ",\"name\":\"Synthetic Oversize Station\",\"totalDocks\":50,\"docksAvailable\":10,"
       "\"bikesAvailable\":10,\"classicBikesAvailable\":10,\"electricBikesAvailable\":0,"
       "\"kioskPublicStatus\":\"Active\",\"bikes\":[";
  bool first = true;
  while (s.size() < StatusStream::kMaxFeatureBytes + 500) {
    if (!first) s += ",";
    first = false;
    s += "{\"dockNumber\":1,\"isElectric\":false,\"isAvailable\":true,\"battery\":null}";
  }
  s += "]}}";  // close bikes[], properties{}, feature{}
  return s;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// Captured 2026-09-14: 5 real Indego stations (ids 3004, 3053, 3150, 3361, 3468).
void test_parse_fixture_filtered() {
  std::vector<uint8_t> body = readFixture("indego_bts_status_sample.json");
  StatusStream s = runStream(body, {3468, 3361}, body.size());

  const auto& st = s.stations();
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(st.size()));

  // Feed order: 3361 appears before 3468 in the fixture.
  TEST_ASSERT_EQUAL_INT(3361, st[0].id);
  TEST_ASSERT_EQUAL_STRING("18th & Fernon, Aquinas Center", st[0].name.c_str());
  TEST_ASSERT_EQUAL_INT(1, st[0].bikes);
  TEST_ASSERT_EQUAL_INT(1, st[0].ebikes);
  TEST_ASSERT_EQUAL_INT(20, st[0].docks);
  TEST_ASSERT_TRUE(st[0].active);

  TEST_ASSERT_EQUAL_INT(3468, st[1].id);
  TEST_ASSERT_EQUAL_STRING("Snyder & Dorrance", st[1].name.c_str());
  TEST_ASSERT_EQUAL_INT(7, st[1].bikes);
  TEST_ASSERT_EQUAL_INT(7, st[1].ebikes);
  TEST_ASSERT_EQUAL_INT(0, st[1].classic);
  TEST_ASSERT_EQUAL_INT(4, st[1].docks);
  TEST_ASSERT_TRUE(st[1].active);

  // The requested filter's last-needed id (3468) is also the last feature in the file, so
  // early abort (if it fires at all) only does so after all 5 features have already closed;
  // featuresSeen() is 5 either way for this particular fixture + filter combination.
  TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(s.featuresSeen()));
}

// Same filter and fixture, split into different chunk sizes: results must be byte-for-byte
// identical regardless of how the body is chunked.
void test_chunk_size_invariance() {
  std::vector<uint8_t> body = readFixture("indego_bts_status_sample.json");
  std::vector<int> filter = {3004, 3053, 3150, 3361, 3468};
  size_t chunk_sizes[] = {1, 7, 100, 4096};

  std::vector<Station> reference;
  for (size_t idx = 0; idx < 4; ++idx) {
    StatusStream s = runStream(body, filter, chunk_sizes[idx]);
    const auto& st = s.stations();
    if (idx == 0) {
      TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(st.size()));
      reference = st;
      continue;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(reference.size()), static_cast<uint32_t>(st.size()),
                                      "station count differs by chunk size");
    for (size_t i = 0; i < reference.size(); ++i) {
      TEST_ASSERT_EQUAL_INT(reference[i].id, st[i].id);
      TEST_ASSERT_EQUAL_STRING(reference[i].name.c_str(), st[i].name.c_str());
      TEST_ASSERT_EQUAL_INT(reference[i].bikes, st[i].bikes);
      TEST_ASSERT_EQUAL_INT(reference[i].ebikes, st[i].ebikes);
      TEST_ASSERT_EQUAL_INT(reference[i].classic, st[i].classic);
      TEST_ASSERT_EQUAL_INT(reference[i].docks, st[i].docks);
      TEST_ASSERT_EQUAL_INT(reference[i].total_docks, st[i].total_docks);
      TEST_ASSERT_EQUAL_INT(reference[i].active, st[i].active);
    }
  }
  // Sanity: feed order matches the fixture's own station order.
  int expected_ids[] = {3004, 3053, 3150, 3361, 3468};
  for (size_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_INT(expected_ids[i], reference[i].id);
}

void test_empty_filter_reports_nothing() {
  std::vector<uint8_t> body = readFixture("indego_bts_status_sample.json");
  StatusStream s;
  s.setStationFilter({});
  TEST_ASSERT_TRUE(s.push(body.data(), body.size()));
  s.finish();
  TEST_ASSERT_TRUE(s.stations().empty());
}

// filter {3004} is the FIRST feature in the file; push() must return false partway through the
// body (well before the ~6.6 KB fixture is fully consumed) once it's found.
void test_early_abort() {
  std::vector<uint8_t> body = readFixture("indego_bts_status_sample.json");
  StatusStream s;
  s.setStationFilter({3004});

  size_t fed = 0;
  bool aborted_early = false;
  const size_t kChunk = 64;
  while (fed < body.size()) {
    size_t n = std::min(kChunk, body.size() - fed);
    bool more = s.push(body.data() + fed, n);
    fed += n;
    if (!more) {
      aborted_early = true;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(aborted_early, "push() never returned false");
  TEST_ASSERT_TRUE_MESSAGE(fed < body.size(), "abort did not happen early");
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(s.stations().size()));
  TEST_ASSERT_EQUAL_INT(3004, s.stations()[0].id);
}

// A synthetic first feature padded past kMaxFeatureBytes, followed by a real feature copied
// (byte for byte) out of the fixture. The oversized one must be skipped without corrupting the
// real one that follows it.
void test_oversize_feature_skipped() {
  std::vector<uint8_t> raw = readFixture("indego_bts_status_sample.json");
  std::string fixture(raw.begin(), raw.end());
  std::string realFeature = extractFeature(fixture, "\"id\":3053");
  TEST_ASSERT_TRUE(realFeature.size() < StatusStream::kMaxFeatureBytes);

  std::string synthetic = "{\"last_updated\":\"x\",\"features\":[" + bigFeature(9999) + "," + realFeature +
                           "],\"type\":\"FeatureCollection\"}";

  StatusStream s;
  s.setStationFilter({9999, 3053});
  s.push(reinterpret_cast<const uint8_t*>(synthetic.data()), synthetic.size());
  s.finish();

  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(s.featuresSkipped()));
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(s.featuresSeen()));

  const auto& st = s.stations();
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(st.size()));
  TEST_ASSERT_EQUAL_INT(3053, st[0].id);
  TEST_ASSERT_EQUAL_STRING("Point Breeze & Tasker", st[0].name.c_str());
  TEST_ASSERT_EQUAL_INT(5, st[0].bikes);
  TEST_ASSERT_EQUAL_INT(5, st[0].ebikes);
  TEST_ASSERT_EQUAL_INT(0, st[0].classic);
  TEST_ASSERT_EQUAL_INT(14, st[0].docks);
  TEST_ASSERT_EQUAL_INT(19, st[0].total_docks);
  TEST_ASSERT_TRUE(st[0].active);
}

// The fixture cut in half lands inside the third feature's JSON (verified offline: exactly 2 of
// the 5 features - ids 3004 and 3053 - are fully received before the halfway point). Truncation
// must not crash, and must report only the features that fully arrived.
void test_truncated_body() {
  std::vector<uint8_t> body = readFixture("indego_bts_status_sample.json");
  std::vector<uint8_t> half(body.begin(), body.begin() + body.size() / 2);

  StatusStream s;
  s.setStationFilter({3004, 3053, 3150, 3361, 3468});
  s.push(half.data(), half.size());
  s.finish();

  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(s.featuresSeen()));
  const auto& st = s.stations();
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(st.size()));
  TEST_ASSERT_EQUAL_INT(3004, st[0].id);
  TEST_ASSERT_EQUAL_INT(3053, st[1].id);
}

// A station name containing escaped quotes and literal braces must not confuse brace-depth
// feature-boundary detection.
void test_braces_and_quotes_in_strings() {
  std::string body =
      "{\"last_updated\":\"x\",\"features\":[{\"geometry\":{\"coordinates\":[-75.0,39.9],\"type\":\"Point\"},"
      "\"properties\":{\"id\":42,\"name\":\"Weird \\\"Name\\\" {with} braces\",\"totalDocks\":5,"
      "\"docksAvailable\":2,\"bikesAvailable\":3,\"classicBikesAvailable\":1,\"electricBikesAvailable\":2,"
      "\"kioskPublicStatus\":\"Active\"}}],\"type\":\"FeatureCollection\"}";

  StatusStream s;
  s.setStationFilter({42});
  s.push(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  s.finish();

  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(s.featuresSeen()));
  const auto& st = s.stations();
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(st.size()));
  TEST_ASSERT_EQUAL_INT(42, st[0].id);
  TEST_ASSERT_EQUAL_STRING("Weird \"Name\" {with} braces", st[0].name.c_str());
  TEST_ASSERT_EQUAL_INT(3, st[0].bikes);
}

void test_status_url() {
  TEST_ASSERT_EQUAL_STRING("https://bts-status.bicycletransit.workers.dev/phl", statusUrl().c_str());
}

// One StatusStream, reused. The firmware allocates the scanner once before Wi-Fi and reset()s it
// per refresh, because its one-feature buffer is a 6,144 B CONTIGUOUS block and the largest free
// block on the target - not the free heap - is what runs out (DESIGN.md 5, "the poll working
// set"). So a reused stream has to produce byte-for-byte what a fresh one does.
void test_reset_rescans_identically_and_keeps_the_buffer() {
  std::vector<uint8_t> body = readFixture("indego_bts_status_sample.json");

  StatusStream fresh;
  fresh.setStationFilter({3468, 3361});
  fresh.push(body.data(), body.size());
  fresh.finish();

  StatusStream reused;
  for (int pass = 0; pass < 3; ++pass) {
    reused.reset();
    // A different filter first, to prove the previous pass's state really is gone.
    reused.setStationFilter({3361});
    reused.push(body.data(), body.size());
    reused.finish();
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(reused.stations().size()));

    reused.reset();
    reused.setStationFilter({3468, 3361});
    reused.push(body.data(), body.size());
    reused.finish();
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fresh.stations().size()),
                              static_cast<uint32_t>(reused.stations().size()));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fresh.featuresSeen()),
                              static_cast<uint32_t>(reused.featuresSeen()));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(fresh.featuresSkipped()),
                              static_cast<uint32_t>(reused.featuresSkipped()));
    for (size_t i = 0; i < fresh.stations().size(); ++i) {
      TEST_ASSERT_EQUAL_INT(fresh.stations()[i].id, reused.stations()[i].id);
      TEST_ASSERT_EQUAL_STRING(fresh.stations()[i].name.c_str(), reused.stations()[i].name.c_str());
      TEST_ASSERT_EQUAL_INT(fresh.stations()[i].bikes, reused.stations()[i].bikes);
      TEST_ASSERT_EQUAL_INT(fresh.stations()[i].ebikes, reused.stations()[i].ebikes);
      TEST_ASSERT_EQUAL_INT(fresh.stations()[i].docks, reused.stations()[i].docks);
    }
  }
}

// A reset in the MIDDLE of a feed must leave no half-captured feature behind.
void test_reset_mid_stream_discards_partial_state() {
  std::vector<uint8_t> body = readFixture("indego_bts_status_sample.json");
  StatusStream s;
  s.setStationFilter({3468, 3361});
  s.push(body.data(), body.size() / 2);  // cut somewhere inside the features array
  s.reset();
  s.setStationFilter({3468, 3361});
  s.push(body.data(), body.size());
  s.finish();
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(s.stations().size()));
  TEST_ASSERT_EQUAL_INT(3361, s.stations()[0].id);
  TEST_ASSERT_EQUAL_INT(3468, s.stations()[1].id);
}

// The buffer borrow (DESIGN.md 5, "the poll working set"). The firmware hands this scanner the
// SAME std::vector the GTFS-RT decoder used as its entity buffer, and each buffered JSON response
// used as its body, earlier in the same cycle - one 6 KB reservation instead of four. So a
// borrowing scanner has to produce exactly what an owning one does, and must not take a buffer of
// its own on top.
void test_borrowed_feature_buffer_scans_identically() {
  std::vector<uint8_t> body = readFixture("indego_bts_status_sample.json");

  StatusStream owning;
  owning.setStationFilter({3468, 3361});
  owning.push(body.data(), body.size());
  owning.finish();

  // Deliberately handed a vector that is EMPTY and unreserved, the way the real one would be after
  // a previous borrower cleared it: setFeatureBuffer sizes it.
  std::vector<uint8_t> shared;
  StatusStream borrowing;
  borrowing.setFeatureBuffer(&shared);
  TEST_ASSERT_TRUE(shared.capacity() >= StatusStream::kMaxFeatureBytes);
  const size_t cap_after_borrow = shared.capacity();

  for (int pass = 0; pass < 3; ++pass) {
    borrowing.reset();
    borrowing.setStationFilter({3468, 3361});
    borrowing.push(body.data(), body.size());
    borrowing.finish();
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(owning.stations().size()),
                              static_cast<uint32_t>(borrowing.stations().size()));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(owning.featuresSeen()),
                              static_cast<uint32_t>(borrowing.featuresSeen()));
    for (size_t i = 0; i < owning.stations().size(); ++i) {
      TEST_ASSERT_EQUAL_INT(owning.stations()[i].id, borrowing.stations()[i].id);
      TEST_ASSERT_EQUAL_STRING(owning.stations()[i].name.c_str(),
                                borrowing.stations()[i].name.c_str());
      TEST_ASSERT_EQUAL_INT(owning.stations()[i].bikes, borrowing.stations()[i].bikes);
    }
    // The borrowed buffer is REUSED, not replaced: a capacity that moved would mean the scanner
    // took storage of its own and the shared reservation had been defeated.
    TEST_ASSERT_EQUAL_UINT32(cap_after_borrow, static_cast<uint32_t>(shared.capacity()));
  }

  // And handing the borrow back puts the scanner on its own buffer again, still correct.
  borrowing.setFeatureBuffer(nullptr);
  borrowing.reset();
  borrowing.setStationFilter({3361});
  borrowing.push(body.data(), body.size());
  borrowing.finish();
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(borrowing.stations().size()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_fixture_filtered);
  RUN_TEST(test_chunk_size_invariance);
  RUN_TEST(test_empty_filter_reports_nothing);
  RUN_TEST(test_early_abort);
  RUN_TEST(test_oversize_feature_skipped);
  RUN_TEST(test_truncated_body);
  RUN_TEST(test_braces_and_quotes_in_strings);
  RUN_TEST(test_status_url);
  RUN_TEST(test_reset_rescans_identically_and_keeps_the_buffer);
  RUN_TEST(test_reset_mid_stream_discards_partial_state);
  RUN_TEST(test_borrowed_feature_buffer_scans_identically);
  return UNITY_END();
}
