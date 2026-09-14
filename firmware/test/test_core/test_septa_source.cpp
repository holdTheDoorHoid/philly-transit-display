#include <unity.h>

#include <algorithm>
#include <map>

#include "fixture_path.h"
#include "transit_core/septa_source.h"

using namespace transit;

namespace {

// Replays a fixed set of fixture files as HTTP responses, keyed by exact URL - a stand-in for
// the real http_fetch.* implementation (owned by the firmware-skeleton work), so
// pollBusStops()/pollRailStops() are testable on host with no network.
HttpGet makeFixtureHttp(std::map<std::string, std::string> url_to_fixture) {
  return [url_to_fixture](const std::string& url,
                           std::function<bool(const uint8_t*, size_t)> onData) -> int {
    auto it = url_to_fixture.find(url);
    if (it == url_to_fixture.end()) return 404;
    std::vector<uint8_t> body = transit_test::readFixture(it->second);
    const size_t kChunk = 4096;
    for (size_t i = 0; i < body.size(); i += kChunk) {
      size_t n = std::min(kChunk, body.size() - i);
      if (!onData(body.data() + i, n)) break;
    }
    return 200;
  };
}

class FakeScheduleCache : public ScheduleCache {
 public:
  bool get(const std::string&, std::vector<SchedEntry>*) override { return false; }
  void put(const std::string& stop_id, const std::vector<SchedEntry>& entries) override {
    stored[stop_id] = entries;
  }
  std::map<std::string, std::vector<SchedEntry>> stored;
};

const StopSnapshot* findStop(const Snapshot& s, const std::string& key) {
  for (const auto& st : s.stops) {
    if (st.key == key) return &st;
  }
  return nullptr;
}

}  // namespace

// --- URL builders ------------------------------------------------------------------------------

void test_septa_url_builders() {
  TEST_ASSERT_EQUAL_STRING("https://www3.septa.org/gtfsrt/septa-pa-us/Trip/rtTripUpdates.pb",
                            septaTripUpdatesUrl().c_str());
  TEST_ASSERT_EQUAL_STRING("https://www3.septa.org/api/TransitView/index.php?route=17",
                            septaTransitViewUrl("17").c_str());
  TEST_ASSERT_EQUAL_STRING("https://www3.septa.org/api/BusSchedules/index.php?stop_id=21332",
                            septaBusSchedulesUrl("21332").c_str());
  TEST_ASSERT_EQUAL_STRING(
      "https://www3.septa.org/api/Arrivals/index.php?station=30th%20Street%20Station",
      septaArrivalsUrl("30th Street Station").c_str());
  TEST_ASSERT_EQUAL_STRING(
      "https://www3.septa.org/api/Arrivals/index.php?station=30th%20Street%20Station&direction=N",
      septaArrivalsUrl("30th Street Station", "N").c_str());
}

// --- Alerts route-id prefix mapping (NOTES.md 7b) ------------------------------------------------

void test_alert_route_id_for_bus_and_trolley() {
  TEST_ASSERT_EQUAL_STRING("bus_route_17", alertRouteIdFor(Mode::Bus, "17").c_str());
  TEST_ASSERT_EQUAL_STRING("bus_route_BLVDDIR", alertRouteIdFor(Mode::Bus, "BLVDDIR").c_str());
  TEST_ASSERT_EQUAL_STRING("trolley_route_10", alertRouteIdFor(Mode::Trolley, "10").c_str());
}

void test_alert_route_id_for_subway_uses_rr_prefix() {
  // Verified live: rr_route_bsl works, bus_route_BSL does not (NOTES.md 7b).
  TEST_ASSERT_EQUAL_STRING("rr_route_bsl", alertRouteIdFor(Mode::Subway, "BSL").c_str());
  TEST_ASSERT_EQUAL_STRING("rr_route_mfl", alertRouteIdFor(Mode::Subway, "MFL").c_str());
}

void test_alert_route_id_for_rail_uses_lookup_table() {
  TEST_ASSERT_EQUAL_STRING("rr_route_fxc", alertRouteIdFor(Mode::Rail, "FOX").c_str());
  TEST_ASSERT_EQUAL_STRING("rr_route_trent", alertRouteIdFor(Mode::Rail, "TRE").c_str());
  TEST_ASSERT_EQUAL_STRING("rr_route_wtren", alertRouteIdFor(Mode::Rail, "WTR").c_str());
  TEST_ASSERT_EQUAL_STRING("", alertRouteIdFor(Mode::Rail, "NOPE").c_str());
}

void test_alert_route_id_for_empty_route() {
  TEST_ASSERT_EQUAL_STRING("", alertRouteIdFor(Mode::Bus, "").c_str());
  TEST_ASSERT_EQUAL_STRING("", septaAlertsUrl(Mode::Bus, "").c_str());
}

// --- pollBusStops --------------------------------------------------------------------------------

void test_poll_bus_stops_merges_both_configured_stops() {
  std::map<std::string, std::string> routes = {
      {septaTripUpdatesUrl(), "septa_bus_tripupdates.pb"},
      {septaTransitViewUrl("17"), "transitview_17.json"},
      {septaBusSchedulesUrl("21332"), "busschedules_21332.json"},
      {septaBusSchedulesUrl("21297"), "busschedules_21297.json"},
  };
  HttpGet http = makeFixtureHttp(routes);
  FakeScheduleCache cache;

  StopConfig sb;
  sb.key = "17-21332";
  sb.mode = Mode::Bus;
  sb.route = "17";
  sb.stop_id = "21332";
  sb.direction = "1";
  sb.headsign = "20th-Johnston";

  StopConfig nb;
  nb.key = "17-21297";
  nb.mode = Mode::Bus;
  nb.route = "17";
  nb.stop_id = "21297";
  nb.direction = "0";
  nb.headsign = "2nd-Market";

  Epoch now = 1789352300;
  Snapshot snap = pollBusStops({sb, nb}, now, http, cache);

  TEST_ASSERT_TRUE(snap.last_poll_ok);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(snap.stops.size()));

  const StopSnapshot* sb_snap = findStop(snap, "17-21332");
  TEST_ASSERT_NOT_NULL(sb_snap);
  bool found_3667 = false;
  for (const auto& a : sb_snap->arrivals) {
    if (a.trip == "3667") {
      found_3667 = true;
      TEST_ASSERT_EQUAL_STRING("7477", a.vehicle.c_str());
      TEST_ASSERT_TRUE(a.status == Status::Live);
    }
  }
  TEST_ASSERT_TRUE(found_3667);

  // Stop 21297 has no GTFS-RT stop_time_update in this fixture (the tracked northbound vehicle
  // hadn't reached it yet at capture time) - it should still show BusSchedules' 4 scheduled
  // entries rather than come back empty.
  const StopSnapshot* nb_snap = findStop(snap, "17-21297");
  TEST_ASSERT_NOT_NULL(nb_snap);
  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(nb_snap->arrivals.size()));
  for (const auto& a : nb_snap->arrivals) {
    TEST_ASSERT_TRUE(a.status == Status::Scheduled);
  }

  // The schedule cache should have been populated for both stops (one fetch each).
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(cache.stored.count("21332")));
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(cache.stored.count("21297")));
}

void test_poll_bus_stops_subway_is_schedule_only() {
  std::map<std::string, std::string> routes = {
      {septaBusSchedulesUrl("1286"), "busschedules_bsl_1286.json"},
  };
  HttpGet http = makeFixtureHttp(routes);
  FakeScheduleCache cache;

  StopConfig subway;
  subway.key = "bsl-snyder";
  subway.mode = Mode::Subway;
  subway.route = "BSL";
  subway.stop_id = "1286";
  subway.direction = "0";

  Epoch now = 1789350000;
  Snapshot snap = pollBusStops({subway}, now, http, cache);

  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.stops.size()));
  const StopSnapshot* s = findStop(snap, "bsl-snyder");
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(s->arrivals.size()));
  for (const auto& a : s->arrivals) {
    TEST_ASSERT_TRUE(a.status == Status::Scheduled);
    TEST_ASSERT_EQUAL_INT64(0, a.predicted);
  }
}

void test_poll_bus_stops_transport_failure_marks_snapshot() {
  HttpGet http = [](const std::string&, std::function<bool(const uint8_t*, size_t)>) -> int {
    return 0;  // simulate DNS/connect failure
  };
  FakeScheduleCache cache;
  StopConfig sb;
  sb.key = "17-21332";
  sb.mode = Mode::Bus;
  sb.route = "17";
  sb.stop_id = "21332";
  sb.direction = "1";

  Snapshot snap = pollBusStops({sb}, 1000, http, cache);
  TEST_ASSERT_FALSE(snap.last_poll_ok);
  TEST_ASSERT_TRUE(snap.last_error.find("TripUpdates") != std::string::npos);
}

// --- pollRailStops -------------------------------------------------------------------------------

void test_poll_rail_stops_merges_by_direction() {
  std::map<std::string, std::string> routes = {
      {septaArrivalsUrl("30th Street Station"), "arrivals_30th.json"},
  };
  HttpGet http = makeFixtureHttp(routes);

  StopConfig north;
  north.key = "rail-30th-N";
  north.mode = Mode::Rail;
  north.station = "30th Street Station";
  north.direction = "N";

  StopConfig south;
  south.key = "rail-30th-S";
  south.mode = Mode::Rail;
  south.station = "30th Street Station";
  south.direction = "S";

  Snapshot snap = pollRailStops({north, south}, 1789352000, http);
  TEST_ASSERT_TRUE(snap.last_poll_ok);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(snap.stops.size()));

  const StopSnapshot* n = findStop(snap, "rail-30th-N");
  const StopSnapshot* s = findStop(snap, "rail-30th-S");
  TEST_ASSERT_NOT_NULL(n);
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(n->arrivals.size()));
  TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(s->arrivals.size()));
}
