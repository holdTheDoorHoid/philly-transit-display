#include <unity.h>

#include "fixture_path.h"
#include "transit_core/gtfsrt_stream.h"
#include "transit_core/merge.h"
#include "transit_core/septa.h"

using namespace transit;

namespace {

std::vector<StopTimeUpdate> decodeTripUpdates(const std::vector<std::string>& routes,
                                               const std::vector<std::string>& stops) {
  auto body = transit_test::readFixture("septa_bus_tripupdates.pb");
  GtfsRtStream stream;
  stream.setRouteFilter(routes);
  stream.setStopFilter(stops);
  std::vector<StopTimeUpdate> out;
  stream.onUpdate([&](const StopTimeUpdate& u) { out.push_back(u); });
  stream.push(body.data(), body.size());
  stream.finish();
  return out;
}

}  // namespace

// The TransitView and TripUpdates fixtures were captured within a minute of each other
// (DESIGN.md / task brief), so trip 3667 is present and joinable in both.
void test_merge_stop_joins_rt_and_tv_by_trip_id() {
  std::vector<StopTimeUpdate> rt = decodeTripUpdates({"17"}, {"21332", "21297"});
  TEST_ASSERT_TRUE(rt.size() > 0);

  auto tv_body = transit_test::readFixture("transitview_17.json");
  ParseResult<TvVehicle> tv = parseTransitView(tv_body.data(), tv_body.size());
  TEST_ASSERT_TRUE(tv.ok);

  auto sched_body = transit_test::readFixture("busschedules_21332.json");
  ParseResult<SchedEntry> sched = parseBusSchedules(sched_body.data(), sched_body.size());
  TEST_ASSERT_TRUE(sched.ok);

  StopConfig cfg;
  cfg.key = "17-21332";
  cfg.mode = Mode::Bus;
  cfg.route = "17";
  cfg.stop_id = "21332";
  cfg.direction = "1";
  cfg.headsign = "20th-Johnston";

  // Just before the TransitView capture's own timestamp - well before the predicted arrival, so
  // nothing here is stale.
  Epoch now = 1789352300;

  StopSnapshot snap = mergeStop(cfg, rt, tv.items, sched.items, now);
  TEST_ASSERT_EQUAL_STRING("17-21332", snap.key.c_str());
  TEST_ASSERT_TRUE(snap.ok);
  TEST_ASSERT_TRUE(snap.arrivals.size() > 0);

  const Arrival* a3667 = nullptr;
  for (const auto& a : snap.arrivals) {
    if (a.trip == "3667") a3667 = &a;
  }
  TEST_ASSERT_NOT_NULL(a3667);
  TEST_ASSERT_EQUAL_STRING("7477", a3667->vehicle.c_str());
  TEST_ASSERT_EQUAL_STRING("20th-Johnston", a3667->destination.c_str());
  TEST_ASSERT_EQUAL_INT64(1789353562, a3667->predicted);
  TEST_ASSERT_EQUAL_INT16(13, a3667->late_min);
  TEST_ASSERT_TRUE(a3667->late_known);
  TEST_ASSERT_TRUE(a3667->status == Status::Live);
  TEST_ASSERT_EQUAL_UINT16(35, a3667->stop_sequence);
  TEST_ASSERT_EQUAL_STRING("EMPTY", a3667->seats.c_str());
  // Nearest BusSchedules entry to (predicted - late*60) = 1789352782 is trip 281757 at
  // 1789352700 (82s away, well inside the +/-600s window) - see the report for the computation.
  TEST_ASSERT_EQUAL_INT64(1789352700, a3667->scheduled);

  // Sorted by effective() ascending.
  for (size_t i = 1; i < snap.arrivals.size(); ++i) {
    TEST_ASSERT_TRUE(snap.arrivals[i - 1].effective() <= snap.arrivals[i].effective());
  }
}

void test_merge_stop_drops_stale_arrivals() {
  std::vector<StopTimeUpdate> rt = decodeTripUpdates({"17"}, {"21332"});
  StopConfig cfg;
  cfg.key = "17-21332";
  cfg.route = "17";
  cfg.stop_id = "21332";
  cfg.direction = "1";

  // Far past every predicted time in the fixture.
  Epoch now = 1789353562 + 10000;
  StopSnapshot snap = mergeStop(cfg, rt, {}, {}, now);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(snap.arrivals.size()));
}

// A Subway-mode config gets empty rt/tv (pollBusStops never fetches GTFS-RT/TransitView for
// subway - NOTES.md 7a) and must fall back to schedule-only display using whatever
// BusSchedules returned (route "B1" for this station, per NOTES.md 7a) - mergeStop matches by
// direction only, not by route id, specifically so this works.
void test_merge_stop_subway_schedule_only() {
  auto sched_body = transit_test::readFixture("busschedules_bsl_1286.json");
  ParseResult<SchedEntry> sched = parseBusSchedules(sched_body.data(), sched_body.size());
  TEST_ASSERT_TRUE(sched.ok);
  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(sched.items.size()));

  StopConfig cfg;
  cfg.key = "bsl-snyder";
  cfg.mode = Mode::Subway;
  cfg.route = "BSL";  // deliberately NOT "B1" - see NOTES.md 7a
  cfg.stop_id = "1286";
  cfg.direction = "0";
  cfg.headsign = "NRG-bound";

  Epoch now = 1789350000;  // before every scheduled time in the fixture
  StopSnapshot snap = mergeStop(cfg, {}, {}, sched.items, now);

  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(snap.arrivals.size()));
  for (const auto& a : snap.arrivals) {
    TEST_ASSERT_TRUE(a.status == Status::Scheduled);
    TEST_ASSERT_EQUAL_STRING("NRG", a.destination.c_str());
    TEST_ASSERT_EQUAL_INT64(0, a.predicted);
    TEST_ASSERT_TRUE(a.scheduled > 0);
  }
  for (size_t i = 1; i < snap.arrivals.size(); ++i) {
    TEST_ASSERT_TRUE(snap.arrivals[i - 1].scheduled <= snap.arrivals[i].scheduled);
  }
}

// --- mergeRail -------------------------------------------------------------------------------

void test_merge_rail_direction_filter_and_status_mapping() {
  auto body = transit_test::readFixture("arrivals_30th.json");
  ParseResult<RailArrival> r = parseRailArrivals(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);

  StopConfig cfg;
  cfg.key = "rail-30th-N";
  cfg.mode = Mode::Rail;
  cfg.station = "30th Street Station";
  cfg.direction = "N";

  Epoch now = 1789352000;
  StopSnapshot snap = mergeRail(cfg, r.items, now);
  TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(snap.arrivals.size()));

  const Arrival* t5878 = nullptr;
  for (const auto& a : snap.arrivals) {
    if (a.trip == "5878") t5878 = &a;
  }
  TEST_ASSERT_NOT_NULL(t5878);
  TEST_ASSERT_EQUAL_STRING("Fox Chase", t5878->destination.c_str());
  TEST_ASSERT_EQUAL_INT64(1789352700, t5878->predicted);  // depart_time
  TEST_ASSERT_EQUAL_INT64(1789352640, t5878->scheduled);  // sched_time
  TEST_ASSERT_TRUE(t5878->late_known);
  TEST_ASSERT_EQUAL_INT16(9, t5878->late_min);  // "9 min"
  TEST_ASSERT_TRUE(t5878->status == Status::Live);
}

void test_merge_rail_on_time_status() {
  auto body = transit_test::readFixture("arrivals_30th.json");
  ParseResult<RailArrival> r = parseRailArrivals(body.data(), body.size());

  StopConfig cfg;
  cfg.key = "rail-30th-S";
  cfg.mode = Mode::Rail;
  cfg.station = "30th Street Station";
  cfg.direction = "S";

  StopSnapshot snap = mergeRail(cfg, r.items, 1789352000);
  const Arrival* t3839 = nullptr;
  for (const auto& a : snap.arrivals) {
    if (a.trip == "3839") t3839 = &a;
  }
  TEST_ASSERT_NOT_NULL(t3839);
  TEST_ASSERT_TRUE(t3839->late_known);
  TEST_ASSERT_EQUAL_INT16(0, t3839->late_min);  // "On Time"
}

void test_merge_rail_line_filter() {
  auto body = transit_test::readFixture("arrivals_30th.json");
  ParseResult<RailArrival> r = parseRailArrivals(body.data(), body.size());

  StopConfig cfg;
  cfg.key = "rail-30th-N-fox";
  cfg.mode = Mode::Rail;
  cfg.station = "30th Street Station";
  cfg.direction = "N";
  cfg.route = "FOX";  // kRailLines code for "Fox Chase"

  StopSnapshot snap = mergeRail(cfg, r.items, 1789352000);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_EQUAL_STRING("5878", snap.arrivals[0].trip.c_str());
}

void test_merge_rail_unrecognized_line_code_matches_nothing() {
  auto body = transit_test::readFixture("arrivals_30th.json");
  ParseResult<RailArrival> r = parseRailArrivals(body.data(), body.size());

  StopConfig cfg;
  cfg.key = "rail-bad-line";
  cfg.mode = Mode::Rail;
  cfg.station = "30th Street Station";
  cfg.route = "NOPE";

  StopSnapshot snap = mergeRail(cfg, r.items, 1789352000);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(snap.arrivals.size()));
}

void test_merge_rail_no_direction_filter_returns_both() {
  auto body = transit_test::readFixture("arrivals_30th.json");
  ParseResult<RailArrival> r = parseRailArrivals(body.data(), body.size());

  StopConfig cfg;
  cfg.key = "rail-both";
  cfg.mode = Mode::Rail;
  cfg.station = "30th Street Station";
  // cfg.direction left empty: both directions.

  StopSnapshot snap = mergeRail(cfg, r.items, 1789352000);
  TEST_ASSERT_EQUAL_UINT32(10, static_cast<uint32_t>(snap.arrivals.size()));
}

// SEPTA's wrong-service-day BusSchedules answers have listed the same static trip id on three
// consecutive days (seen live 2026-09-14 for stop 21297: trip 280824 at +887, +2327, +3767 min).
// One row per trip id, earliest copy wins.
void test_merge_stop_dedupes_scheduled_rows_by_trip_id() {
  StopConfig cfg;
  cfg.key = "17-21297";
  cfg.mode = Mode::Bus;
  cfg.route = "17";
  cfg.stop_id = "21297";
  cfg.direction = "0";
  Epoch now = 1789351200;
  std::vector<SchedEntry> sched;
  for (int day = 2; day >= 0; --day) {  // deliberately out of order: latest copy first
    SchedEntry e;
    e.route = "17";
    e.trip_id = "280824";
    e.direction = "0";
    e.direction_desc = "2nd-Market";
    e.scheduled = now + 600 + day * 86400;
    sched.push_back(e);
  }
  SchedEntry other;
  other.route = "17";
  other.trip_id = "280909";
  other.direction = "0";
  other.scheduled = now + 1800;
  sched.push_back(other);

  StopSnapshot snap = mergeStop(cfg, {}, {}, sched, now);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_EQUAL_STRING("280824", snap.arrivals[0].trip.c_str());
  TEST_ASSERT_EQUAL_INT64(now + 600, snap.arrivals[0].scheduled);
  TEST_ASSERT_EQUAL_STRING("280909", snap.arrivals[1].trip.c_str());
}
