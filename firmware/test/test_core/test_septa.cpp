#include <unity.h>

#include "fixture_path.h"
#include "transit_core/septa.h"

using namespace transit;

namespace {
const TvVehicle* findTv(const std::vector<TvVehicle>& v, const std::string& trip) {
  for (const auto& x : v) {
    if (x.trip == trip) return &x;
  }
  return nullptr;
}
}  // namespace

// --- TransitView -----------------------------------------------------------------------------

void test_parse_transitview_fixture() {
  auto body = transit_test::readFixture("transitview_17.json");
  ParseResult<TvVehicle> r = parseTransitView(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(r.items.size()));

  const TvVehicle* v = findTv(r.items, "3667");
  TEST_ASSERT_NOT_NULL(v);
  TEST_ASSERT_EQUAL_STRING("7477", v->vehicle_id.c_str());
  TEST_ASSERT_EQUAL_INT(13, v->late);
  TEST_ASSERT_EQUAL_STRING("20th-Johnston", v->destination.c_str());
  TEST_ASSERT_EQUAL_STRING("Southbound", v->direction.c_str());
  TEST_ASSERT_EQUAL_STRING("10338", v->next_stop_id.c_str());
  TEST_ASSERT_EQUAL_UINT32(5, v->next_stop_sequence);
  TEST_ASSERT_EQUAL_STRING("EMPTY", v->seats.c_str());
  TEST_ASSERT_EQUAL_INT64(1789352300, v->timestamp);
  TEST_ASSERT_FLOAT_WITHIN(0.0001, 39.950945, v->lat);
  TEST_ASSERT_FLOAT_WITHIN(0.0001, -75.151433, v->lng);
}

void test_parse_transitview_bare_empty_array_is_not_an_error() {
  auto body = transit_test::readFixture("transitview_BSL.json");  // literally "[]"
  ParseResult<TvVehicle> r = parseTransitView(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(r.items.size()));
}

// --- BusSchedules ------------------------------------------------------------------------------

void test_parse_bus_schedules_fixture() {
  auto body = transit_test::readFixture("busschedules_21332.json");
  ParseResult<SchedEntry> r = parseBusSchedules(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(r.items.size()));

  const SchedEntry& e = r.items[0];
  TEST_ASSERT_EQUAL_STRING("17", e.route.c_str());
  TEST_ASSERT_EQUAL_STRING("281757", e.trip_id.c_str());
  TEST_ASSERT_EQUAL_STRING("1", e.direction.c_str());
  TEST_ASSERT_EQUAL_STRING("20th-Johnston", e.direction_desc.c_str());
  TEST_ASSERT_EQUAL_STRING("19th St & Mifflin St", e.stop_name.c_str());
  TEST_ASSERT_TRUE(e.scheduled > 0);
}

void test_parse_bus_schedules_error_400_shape() {
  auto body = transit_test::readFixture("busschedules_error_400.json");
  ParseResult<SchedEntry> r = parseBusSchedules(body.data(), body.size());
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.error.find("invalid parameter") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(r.items.size()));
}

void test_parse_bus_schedules_501_body_is_actually_valid_shaped() {
  // See NOTES.md 1: this fixture was captured during a genuine HTTP 501 response, but the body
  // itself is ordinary, well-formed schedule JSON - SEPTA's flakiness is in the status code, not
  // always the payload. parseBusSchedules only looks at the body shape, so this parses fine;
  // deciding whether to trust a non-200 status is the caller's job (septa_source.cpp).
  auto body = transit_test::readFixture("busschedules_error_501.json");
  ParseResult<SchedEntry> r = parseBusSchedules(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.items.size() > 0);
  TEST_ASSERT_EQUAL_STRING("17", r.items[0].route.c_str());
}

void test_parse_bus_schedules_subway_route_id() {
  // NOTES.md 7a: a subway station's BusSchedules response reports route "B1", not "BSL".
  auto body = transit_test::readFixture("busschedules_bsl_1286.json");
  ParseResult<SchedEntry> r = parseBusSchedules(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.items.size() > 0);
  TEST_ASSERT_EQUAL_STRING("B1", r.items[0].route.c_str());
  TEST_ASSERT_EQUAL_STRING("Snyder", r.items[0].stop_name.c_str());
  TEST_ASSERT_EQUAL_STRING("NRG", r.items[0].direction_desc.c_str());
}

// --- Alerts --------------------------------------------------------------------------------

void test_parse_alerts_fixture() {
  auto body = transit_test::readFixture("alerts_bus_17.json");
  ParseResult<Alert> r = parseAlerts(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(r.items.size()));

  const Alert& a = r.items[0];
  TEST_ASSERT_EQUAL_STRING("17", a.route.c_str());
  // HTML must be stripped: no '<' left, and the heading text should appear in plain text.
  TEST_ASSERT_TRUE(a.text.find('<') == std::string::npos);
  TEST_ASSERT_TRUE(a.text.find("Route 17 is shortened") != std::string::npos);
  TEST_ASSERT_TRUE(a.text.size() <= 240);
  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(a.detours.size()));
  TEST_ASSERT_TRUE(a.detours[0].find("Construction") != std::string::npos);
  TEST_ASSERT_TRUE(a.current);
}

void test_parse_alerts_empty_array() {
  ParseResult<Alert> r = parseAlerts(reinterpret_cast<const uint8_t*>("[]"), 2);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(r.items.size()));
}

// --- Arrivals (Regional Rail) ----------------------------------------------------------------

void test_parse_rail_arrivals_fixture() {
  auto body = transit_test::readFixture("arrivals_30th.json");
  ParseResult<RailArrival> r = parseRailArrivals(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(10, static_cast<uint32_t>(r.items.size()));  // 5 N + 5 S

  int north = 0, south = 0;
  const RailArrival* first_n = nullptr;
  const RailArrival* first_s = nullptr;
  for (const auto& t : r.items) {
    if (t.direction == "N") {
      north++;
      if (!first_n) first_n = &t;
    } else if (t.direction == "S") {
      south++;
      if (!first_s) first_s = &t;
    }
  }
  TEST_ASSERT_EQUAL_INT(5, north);
  TEST_ASSERT_EQUAL_INT(5, south);

  TEST_ASSERT_NOT_NULL(first_n);
  TEST_ASSERT_EQUAL_STRING("5878", first_n->train_id.c_str());
  TEST_ASSERT_EQUAL_STRING("Fox Chase", first_n->line.c_str());
  TEST_ASSERT_EQUAL_STRING("Fox Chase", first_n->destination.c_str());
  TEST_ASSERT_EQUAL_STRING("Malvern", first_n->origin.c_str());
  TEST_ASSERT_EQUAL_STRING("9 min", first_n->status.c_str());
  TEST_ASSERT_EQUAL_INT64(1789352640, first_n->sched);
  TEST_ASSERT_EQUAL_INT64(1789352700, first_n->depart);
  TEST_ASSERT_EQUAL_STRING("1", first_n->track.c_str());

  TEST_ASSERT_NOT_NULL(first_s);
  TEST_ASSERT_EQUAL_STRING("3839", first_s->train_id.c_str());
  TEST_ASSERT_EQUAL_STRING("On Time", first_s->status.c_str());
  TEST_ASSERT_EQUAL_INT64(1789352580, first_s->sched);
  TEST_ASSERT_EQUAL_INT64(1789352640, first_s->depart);
}

void test_parse_rail_arrivals_error_shape() {
  static const char kBody[] =
      "{\"error\": \"An invalid parameter was used. Ensure 'req1' is assigned a valid Regional "
      "Rail station name.\"}";
  ParseResult<RailArrival> r =
      parseRailArrivals(reinterpret_cast<const uint8_t*>(kBody), sizeof(kBody) - 1);
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.error.find("invalid parameter") != std::string::npos);
}

// --- Rail line reference table -----------------------------------------------------------------

void test_rail_line_lookup() {
  const RailLine* fox = findRailLine("FOX");
  TEST_ASSERT_NOT_NULL(fox);
  TEST_ASSERT_EQUAL_STRING("Fox Chase", fox->display_name);
  TEST_ASSERT_EQUAL_STRING("fxc", fox->alert_suffix);

  const RailLine* tre = findRailLine("TRE");
  TEST_ASSERT_NOT_NULL(tre);
  TEST_ASSERT_EQUAL_STRING("trent", tre->alert_suffix);  // NOT "tre" - see NOTES.md 7b

  TEST_ASSERT_NULL(findRailLine("NOPE"));
}
