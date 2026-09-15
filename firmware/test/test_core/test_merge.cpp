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

// =============================================================================================
// Regression tests for the 2026-09-15 adversarial review. Each asserts the CORRECT behaviour;
// the review's own harness asserted the defective one.
// =============================================================================================

namespace {

// A BusSchedules entry, built inline rather than from a fixture so the route/direction/time
// combination under test is unambiguous.
SchedEntry sched(const std::string& route, const std::string& trip_id, const std::string& dir,
                  Epoch when, const std::string& desc = "") {
  SchedEntry e;
  e.route = route;
  e.trip_id = trip_id;
  e.direction = dir;
  e.direction_desc = desc;
  e.scheduled = when;
  return e;
}

// One GTFS-RT stop_time_update. `arrival` of 0 means "the feed gave no time for this stop".
StopTimeUpdate rtUpdate(const std::string& trip, const std::string& route, const std::string& stop,
                         int dir, Epoch arrival, Epoch feed_ts = 0) {
  StopTimeUpdate u;
  u.trip_id = trip;
  u.route_id = route;
  u.stop_id = stop;
  u.direction_id = dir;
  u.feed_timestamp = feed_ts;
  if (arrival != 0) {
    u.arrival_time = arrival;
    u.has_arrival_time = true;
  }
  return u;
}

StopConfig busCfg(const std::string& route, const std::string& stop, const std::string& dir) {
  StopConfig c;
  c.key = route + "-" + stop;
  c.mode = Mode::Bus;
  c.route = route;
  c.stop_id = stop;
  c.direction = dir;
  return c;
}

}  // namespace

// --- F14: schedules from another route must not be merged into this route's panel -------------

// Reproduction from the review: cfg.route "17" plus a SchedEntry for route "2" in the same
// direction used to come back as one arrival, because `sched` was filtered by direction only.
void test_merge_stop_ignores_other_route_schedule_entries() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<SchedEntry> s = {sched("2", "999001", "1", now + 600, "Other Route")};

  StopSnapshot snap = mergeStop(cfg, {}, {}, s, now);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(snap.arrivals.size()));

  // ...and the same stop's own route still works, so this is a filter, not a blanket refusal.
  s.push_back(sched("17", "281757", "1", now + 900, "20th-Johnston"));
  StopSnapshot snap2 = mergeStop(cfg, {}, {}, s, now);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap2.arrivals.size()));
  TEST_ASSERT_EQUAL_STRING("281757", snap2.arrivals[0].trip.c_str());
}

// The other half of F14: a live route-17 arrival used to consume route 2's scheduled time, which
// both corrupted its "scheduled" column and hid route 2's own row.
void test_merge_stop_live_arrival_does_not_match_other_route_schedule() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<StopTimeUpdate> rt = {rtUpdate("3667", "17", "21332", 1, now + 300)};
  std::vector<SchedEntry> s = {sched("2", "999001", "1", now + 300, "Other Route")};

  StopSnapshot snap = mergeStop(cfg, rt, {}, s, now);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_EQUAL_STRING("3667", snap.arrivals[0].trip.c_str());
  TEST_ASSERT_EQUAL_INT64(0, snap.arrivals[0].scheduled);        // no cross-route match
  TEST_ASSERT_EQUAL_STRING("", snap.arrivals[0].sched_trip.c_str());
}

// Route matching is case-insensitive for bus/trolley (SEPTA's letter routes are upper case in
// every feed seen, but the config is user-entered).
void test_sched_route_matches_is_case_insensitive_for_bus() {
  StopConfig cfg = busCfg("t4", "1234", "0");
  TEST_ASSERT_TRUE(schedRouteMatches(cfg, sched("T4", "1", "0", 0)));
  TEST_ASSERT_FALSE(schedRouteMatches(cfg, sched("T5", "1", "0", 0)));
  cfg.route.clear();  // no route configured: no filter
  TEST_ASSERT_TRUE(schedRouteMatches(cfg, sched("anything", "1", "0", 0)));
}

// Subway must keep working through the alias map: the fixture busschedules_bsl_1286.json is
// keyed "B1" for a station configured as "BSL" (NOTES.md 7a).
void test_merge_stop_subway_route_alias_from_fixture() {
  auto sched_body = transit_test::readFixture("busschedules_bsl_1286.json");
  ParseResult<SchedEntry> parsed = parseBusSchedules(sched_body.data(), sched_body.size());
  TEST_ASSERT_TRUE(parsed.ok);
  TEST_ASSERT_EQUAL_STRING("B1", parsed.items[0].route.c_str());  // the alias form, as captured

  StopConfig cfg;
  cfg.key = "bsl-snyder";
  cfg.mode = Mode::Subway;
  cfg.route = "BSL";
  cfg.stop_id = "1286";
  cfg.direction = "0";
  TEST_ASSERT_TRUE(schedRouteMatches(cfg, parsed.items[0]));

  StopSnapshot snap = mergeStop(cfg, {}, {}, parsed.items, 1789350000);
  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_TRUE(snap.health == Health::ScheduleOnly);

  // A subway station whose schedule came back under some *other* line's id is still rejected.
  SchedEntry foreign = sched("L1", "700001", "0", 1789350600);
  TEST_ASSERT_FALSE(schedRouteMatches(cfg, foreign));
}

// A subway line whose GTFS route ids this project never verified (NHSL, the owl variants) takes
// whatever BusSchedules returned for the station - see schedRouteMatches() for why that is safe
// for subway specifically and is not extended to bus or trolley.
void test_sched_route_matches_unknown_subway_accepts_any() {
  StopConfig cfg;
  cfg.mode = Mode::Subway;
  cfg.route = "NHSL";
  TEST_ASSERT_TRUE(schedRouteMatches(cfg, sched("NHS", "1", "0", 0)));

  StopConfig bus = busCfg("NHSL", "1", "0");  // same id, bus mode: no such licence
  TEST_ASSERT_FALSE(schedRouteMatches(bus, sched("NHS", "1", "0", 0)));
}

// The matched static trip id is kept on the arrival so transit_stats can reconcile the scheduled
// and live records for one trip later.
void test_merge_stop_keeps_matched_static_trip_id() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<StopTimeUpdate> rt = {rtUpdate("3667", "17", "21332", 1, now + 300)};
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 240, "20th-Johnston"),
                               sched("17", "281756", "1", now + 1800, "20th-Johnston")};

  StopSnapshot snap = mergeStop(cfg, rt, {}, s, now);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_EQUAL_STRING("281757", snap.arrivals[0].sched_trip.c_str());  // live row, matched
  TEST_ASSERT_EQUAL_INT64(now + 240, snap.arrivals[0].scheduled);
  // A schedule-only row is its own static trip.
  TEST_ASSERT_EQUAL_STRING("281756", snap.arrivals[1].sched_trip.c_str());
  TEST_ASSERT_EQUAL_STRING("281756", snap.arrivals[1].trip.c_str());
}

// --- F16: skipped and cancelled trips must not reappear as ordinary arrivals ------------------

// The review's exact case: a SKIPPED update with no predicted time used to make a zero-time row
// (deleted as stale), after which its scheduled counterpart was printed as a normal bus.
void test_merge_stop_skipped_without_time_is_shown_and_consumes_schedule() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  StopTimeUpdate u = rtUpdate("3667", "17", "21332", 1, 0);
  u.schedule_relationship = static_cast<uint8_t>(StopRel::Skipped);
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 600, "20th-Johnston")};

  StopSnapshot snap = mergeStop(cfg, {u}, {}, s, now);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_TRUE(snap.arrivals[0].status == Status::Skipped);
  TEST_ASSERT_EQUAL_STRING("3667", snap.arrivals[0].trip.c_str());
  TEST_ASSERT_EQUAL_INT64(0, snap.arrivals[0].predicted);        // no time invented
  TEST_ASSERT_EQUAL_INT64(now + 600, snap.arrivals[0].scheduled);  // shown at the scheduled time
  TEST_ASSERT_EQUAL_STRING("281757", snap.arrivals[0].sched_trip.c_str());
}

// Same trip, but the feed did give a time: the row keeps it and still matches its schedule entry.
void test_merge_stop_skipped_with_time_keeps_prediction() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  StopTimeUpdate u = rtUpdate("3667", "17", "21332", 1, now + 300);
  u.schedule_relationship = static_cast<uint8_t>(StopRel::Skipped);
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 240, "20th-Johnston")};

  StopSnapshot snap = mergeStop(cfg, {u}, {}, s, now);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_TRUE(snap.arrivals[0].status == Status::Skipped);
  TEST_ASSERT_EQUAL_INT64(now + 300, snap.arrivals[0].predicted);
  TEST_ASSERT_EQUAL_INT64(now + 240, snap.arrivals[0].scheduled);
}

// A trip-level CANCELED must not be displayed at all, and must suppress the schedule row it
// would otherwise have been matched to.
void test_merge_stop_canceled_trip_suppresses_its_schedule_row() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  StopTimeUpdate u = rtUpdate("3667", "17", "21332", 1, now + 300);
  u.trip_schedule_relationship = static_cast<uint8_t>(TripRel::Canceled);
  TEST_ASSERT_TRUE(u.tripCanceled());
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 300, "20th-Johnston"),
                               sched("17", "281756", "1", now + 1800, "20th-Johnston")};

  StopSnapshot snap = mergeStop(cfg, {u}, {}, s, now);
  // The cancelled trip's own row and its scheduled counterpart are both gone; the later,
  // unrelated trip is untouched.
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_EQUAL_STRING("281756", snap.arrivals[0].trip.c_str());
  TEST_ASSERT_TRUE(snap.arrivals[0].status == Status::Scheduled);
}

// NO_DATA is not a prediction: no time is invented, and the schedule row stands in for it.
void test_merge_stop_no_data_leaves_the_schedule_row() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  StopTimeUpdate u = rtUpdate("3667", "17", "21332", 1, 0);
  u.schedule_relationship = static_cast<uint8_t>(StopRel::NoData);
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 600, "20th-Johnston")};

  StopSnapshot snap = mergeStop(cfg, {u}, {}, s, now);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_TRUE(snap.arrivals[0].status == Status::Scheduled);
  TEST_ASSERT_EQUAL_STRING("281757", snap.arrivals[0].trip.c_str());
  TEST_ASSERT_EQUAL_INT64(0, snap.arrivals[0].predicted);
  TEST_ASSERT_EQUAL_INT64(now + 600, snap.arrivals[0].scheduled);
  TEST_ASSERT_TRUE(snap.health == Health::ScheduleOnly);
}

// The fourth shape, for contrast with the three above: no realtime at all for this trip.
void test_merge_stop_plain_schedule_row_is_distinct() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 600, "20th-Johnston")};

  StopSnapshot snap = mergeStop(cfg, {}, {}, s, now);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_TRUE(snap.arrivals[0].status == Status::Scheduled);
  TEST_ASSERT_EQUAL_STRING("20th-Johnston", snap.arrivals[0].destination.c_str());
  TEST_ASSERT_TRUE(snap.ok);
}

// --- F15: a feed is only live while its own timestamp says so ---------------------------------

void test_merge_stop_fresh_feed_is_live() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<StopTimeUpdate> rt = {rtUpdate("3667", "17", "21332", 1, now + 300, now - 20)};
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 240, "20th-Johnston")};

  StopSnapshot snap = mergeStop(cfg, rt, {}, s, now);
  TEST_ASSERT_TRUE(snap.health == Health::Live);
  TEST_ASSERT_TRUE(snap.ok);
  TEST_ASSERT_EQUAL_INT64(now - 20, snap.source_ts);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_TRUE(snap.arrivals[0].status == Status::Live);
}

// A replayed/cached feed: well-formed, full of future predictions, and half an hour old.
void test_merge_stop_old_feed_is_stale_with_no_live_rows() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<StopTimeUpdate> rt = {rtUpdate("3667", "17", "21332", 1, now + 300, now - 1800)};
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 240, "20th-Johnston")};

  StopSnapshot snap = mergeStop(cfg, rt, {}, s, now);
  TEST_ASSERT_TRUE(snap.health == Health::Stale);
  TEST_ASSERT_FALSE(snap.ok);
  TEST_ASSERT_EQUAL_INT64(now - 1800, snap.source_ts);
  for (const auto& a : snap.arrivals) {
    TEST_ASSERT_TRUE(a.status != Status::Live);
    TEST_ASSERT_EQUAL_INT64(0, a.predicted);  // the stale prediction itself is not shown
  }
  // The matched schedule time survives as an ordinary scheduled row.
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_TRUE(snap.arrivals[0].status == Status::Scheduled);
  TEST_ASSERT_EQUAL_INT64(now + 240, snap.arrivals[0].scheduled);
}

void test_merge_stop_future_dated_feed_is_stale() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<StopTimeUpdate> rt = {rtUpdate("3667", "17", "21332", 1, now + 300, now + 3600)};
  StopSnapshot snap = mergeStop(cfg, rt, {}, {}, now);
  TEST_ASSERT_TRUE(snap.health == Health::Stale);
  TEST_ASSERT_FALSE(snap.ok);
  // No schedule to fall back on, so the untrustworthy row is dropped rather than shown.
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(snap.arrivals.size()));
}

// A feed with no header timestamp is "unknown age", not "stale" - see merge.h.
void test_merge_stop_missing_feed_timestamp_keeps_live_rows() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<StopTimeUpdate> rt = {rtUpdate("3667", "17", "21332", 1, now + 300, 0)};
  StopSnapshot snap = mergeStop(cfg, rt, {}, {}, now);
  TEST_ASSERT_TRUE(snap.health == Health::Live);
  TEST_ASSERT_EQUAL_INT64(now, snap.source_ts);  // best honest answer: when we assembled it
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
}

// --- F13: a failed source has to reach the stop it belongs to ---------------------------------

void test_merge_stop_subway_schedule_failure_is_unavailable() {
  StopConfig cfg;
  cfg.key = "bsl-snyder";
  cfg.mode = Mode::Subway;
  cfg.route = "BSL";
  cfg.stop_id = "1286";
  SourceStatus bad;
  bad.schedule_ok = false;

  StopSnapshot snap = mergeStop(cfg, {}, {}, {}, 1789351200, bad);
  TEST_ASSERT_FALSE(snap.ok);
  TEST_ASSERT_TRUE(snap.health == Health::Unavailable);
  TEST_ASSERT_TRUE(snap.error.find("schedule") != std::string::npos);

  // A subway stop has no realtime source, so a TripUpdates failure must not touch it.
  SourceStatus rt_down;
  rt_down.live_ok = false;
  rt_down.vehicles_ok = false;
  StopSnapshot fine = mergeStop(cfg, {}, {}, {}, 1789351200, rt_down);
  TEST_ASSERT_TRUE(fine.ok);
}

// A bus stop whose live feed was truncated still shows its schedule - flagged, not silent.
void test_merge_stop_live_failure_falls_back_to_schedule_only() {
  StopConfig cfg = busCfg("17", "21332", "1");
  Epoch now = 1789351200;
  std::vector<SchedEntry> s = {sched("17", "281757", "1", now + 600, "20th-Johnston")};
  SourceStatus bad;
  bad.live_ok = false;

  StopSnapshot snap = mergeStop(cfg, {}, {}, s, now, bad);
  TEST_ASSERT_FALSE(snap.ok);
  TEST_ASSERT_TRUE(snap.health == Health::ScheduleOnly);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_TRUE(snap.error.find("live feed") != std::string::npos);
}

// --- F30: a rail config that still carries the display name keeps working ---------------------

void test_merge_rail_accepts_line_display_name_in_config() {
  auto body = transit_test::readFixture("arrivals_30th.json");
  ParseResult<RailArrival> r = parseRailArrivals(body.data(), body.size());

  StopConfig cfg;
  cfg.key = "rail-30th-N-fox";
  cfg.mode = Mode::Rail;
  cfg.station = "30th Street Station";
  cfg.direction = "N";
  cfg.route = "Fox Chase";  // the display name, not the "FOX" code

  StopSnapshot snap = mergeRail(cfg, r.items, 1789352000);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_EQUAL_STRING("5878", snap.arrivals[0].trip.c_str());
  TEST_ASSERT_TRUE(snap.ok);
  TEST_ASSERT_EQUAL_INT64(1789352000, snap.source_ts);
}

void test_merge_rail_failed_fetch_marks_the_stop() {
  StopConfig cfg;
  cfg.key = "rail-30th-N";
  cfg.mode = Mode::Rail;
  cfg.station = "30th Street Station";
  SourceStatus bad;
  bad.live_ok = false;

  StopSnapshot snap = mergeRail(cfg, {}, 1789352000, bad);
  TEST_ASSERT_FALSE(snap.ok);
  TEST_ASSERT_TRUE(snap.health == Health::Unavailable);
  TEST_ASSERT_TRUE(snap.error.size() > 0);
}

// --- F32: the rail status minute parser must not overflow -------------------------------------

namespace {

// Runs one RailArrival::status string through mergeRail and reports what it made of it.
void checkStatus(const char* status, bool expect_known, int expect_min) {
  RailArrival ra;
  ra.direction = "N";
  ra.train_id = "1";
  ra.depart = 2000000000;
  ra.sched = 2000000000;
  ra.status = status;

  StopConfig cfg;
  cfg.key = "rail";
  cfg.mode = Mode::Rail;
  StopSnapshot snap = mergeRail(cfg, {ra}, 1999999000);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(snap.arrivals.size()));
  TEST_ASSERT_EQUAL_INT(expect_known ? 1 : 0, snap.arrivals[0].late_known ? 1 : 0);
  if (expect_known) TEST_ASSERT_EQUAL_INT16(expect_min, snap.arrivals[0].late_min);
}

}  // namespace

void test_parse_signed_minutes_bounds_and_garbage() {
  // The values that must keep working.
  checkStatus("On Time", true, 0);
  checkStatus("3 min", true, 3);
  checkStatus("-2 min", true, -2);
  checkStatus("13 minutes", true, 13);
  checkStatus("1440 min", true, 1440);   // the largest lateness this code will believe
  checkStatus("-1440 mins", true, -1440);

  // Out of range, and the digit string that used to be signed-overflow UB (confirmed by the
  // review with UBSan). Both must simply read as "lateness not known", never as a number.
  checkStatus("1441 min", false, 0);
  checkStatus("99999 min", false, 0);
  checkStatus("999999999999999999999999999 min", false, 0);
  checkStatus("-999999999999999999999999999 min", false, 0);

  // Shapes that are not a minute count at all.
  checkStatus("Delayed", false, 0);
  checkStatus("Suspended", false, 0);
  checkStatus("3 min later", false, 0);  // trailing garbage
  checkStatus("3min", false, 0);
  checkStatus("min", false, 0);
  checkStatus("", false, 0);
}
