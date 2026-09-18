#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "fixture_path.h"
#include "transit_core/septa.h"
#include "transit_core/timeparse.h"

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

// =============================================================================================
// Regression tests for the 2026-09-15 adversarial review (F06 parser retention caps, F30 rail
// line lookup by display name, F32 checked number parsing).
// =============================================================================================

namespace {

// Builds a BusSchedules body with `count` entries, the i-th scheduled i minutes after the
// fixtures' reference evening. Entry 0 is the soonest.
std::string makeSchedulesBody(int count) {
  std::string out = "{\"17\":[";
  for (int i = 0; i < count; ++i) {
    if (i) out += ",";
    int hour = 1 + (i / 60);        // 1:00 am onwards, so they stay in one day and in order
    int minute = i % 60;
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{\"StopName\":\"S\",\"Route\":\"17\",\"trip_id\":\"%d\",\"date\":\"x\","
                  "\"day\":\"Mon\",\"Direction\":\"0\","
                  "\"DateCalender\":\"09/14/26 %02d:%02d am\",\"DirectionDesc\":\"D\"}",
                  1000 + i, hour, minute);
    out += buf;
  }
  out += "]}";
  return out;
}

ParseResult<SchedEntry> parseSchedulesString(const std::string& s) {
  return parseBusSchedules(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

ParseResult<TvVehicle> parseTvString(const std::string& s) {
  return parseTransitView(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

ParseResult<TvVehicle> parseTvString(const std::string& s, const TvFilter& filter) {
  ParseResult<TvVehicle> r;
  parseTransitViewAppend(&r, reinterpret_cast<const uint8_t*>(s.data()), s.size(), filter);
  return r;
}

// A TvFilter over an explicit list of trip ids - the shape pollBusStops() builds from the
// retained GTFS-RT updates, with the vector spelled out instead of decoded from a feed.
bool tripIsListed(const std::string& trip, const void* ctx) {
  const auto* wanted = static_cast<const std::vector<std::string>*>(ctx);
  for (const auto& t : *wanted) {
    if (t == trip) return true;
  }
  return false;
}

bool tripIsNeverWanted(const std::string&, const void*) { return false; }

}  // namespace

// A real BusSchedules answer is 4-12 entries; nothing stops it from being thousands, and an
// unbounded vector of those is a dead device (DESIGN.md 12.1).
void test_parse_bus_schedules_caps_entries_and_keeps_the_nearest() {
  ParseResult<SchedEntry> r = parseSchedulesString(makeSchedulesBody(400));
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMaxSchedEntries),
                            static_cast<uint32_t>(r.items.size()));
  TEST_ASSERT_EQUAL_UINT32(400 - static_cast<uint32_t>(kMaxSchedEntries), r.dropped);

  // The survivors are the soonest ones: entries 0..23, i.e. 1:00 am through 1:23 am.
  Epoch latest = 0;
  for (const auto& e : r.items) {
    TEST_ASSERT_TRUE(e.scheduled > 0);
    if (e.scheduled > latest) latest = e.scheduled;
  }
  Epoch first_ok = parseBusScheduleTime("09/14/26 01:00 am");
  TEST_ASSERT_EQUAL_INT64(first_ok + (static_cast<Epoch>(kMaxSchedEntries) - 1) * 60, latest);

  // A normal-sized answer is untouched, in wire order, with nothing dropped.
  ParseResult<SchedEntry> small = parseSchedulesString(makeSchedulesBody(4));
  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(small.items.size()));
  TEST_ASSERT_EQUAL_UINT32(0, small.dropped);
  TEST_ASSERT_EQUAL_STRING("1000", small.items[0].trip_id.c_str());
}

void test_parse_transitview_caps_vehicles() {
  std::string body = "{\"bus\":[";
  for (size_t i = 0; i < kMaxTvVehicles + 20; ++i) {
    if (i) body += ",";
    body += "{\"trip\":\"" + std::to_string(i) + "\",\"VehicleID\":\"v\",\"late\":0}";
  }
  body += "]}";
  ParseResult<TvVehicle> r = parseTvString(body);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMaxTvVehicles),
                            static_cast<uint32_t>(r.items.size()));
  TEST_ASSERT_EQUAL_UINT32(20, r.dropped);
}

// Long identifiers are truncated rather than retained whole.
void test_parse_transitview_truncates_long_identifiers() {
  std::string huge(4000, 'x');
  std::string body = "{\"bus\":[{\"trip\":\"" + huge + "\",\"destination\":\"" + huge + "\"}]}";
  ParseResult<TvVehicle> r = parseTvString(body);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(r.items.size()));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMaxIdChars),
                            static_cast<uint32_t>(r.items[0].trip.size()));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMaxIdChars),
                            static_cast<uint32_t>(r.items[0].destination.size()));
}

// SEPTA quotes some numeric fields and not others (NOTES.md 3); a quoted one long enough to
// overflow used to go through atoll(), which is undefined behaviour out of range.
void test_parse_transitview_absurd_numbers_do_not_overflow() {
  std::string body =
      "{\"bus\":[{\"trip\":\"1\",\"late\":\"999999999999999999999999999\","
      "\"next_stop_sequence\":\"-99999999999999999999\",\"timestamp\":\"12abc\","
      "\"lat\":\"39.95junk\",\"lng\":\"-75.17\"}]}";
  ParseResult<TvVehicle> r = parseTvString(body);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(r.items.size()));
  TEST_ASSERT_EQUAL_INT(0, r.items[0].late);        // unparseable reads as absent, like a null
  TEST_ASSERT_EQUAL_INT64(0, r.items[0].timestamp); // trailing garbage is not a number
  TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.0, r.items[0].lat);      // ditto
  TEST_ASSERT_FLOAT_WITHIN(0.0001, -75.17, r.items[0].lng);  // a well-formed one still parses
}

// F30: the config layer needs one lookup that takes either spelling, so a config still carrying
// the display name SEPTA itself reports keeps filtering correctly.
void test_rail_line_lookup_by_code_or_display_name() {
  const RailLine* by_code = findRailLineByName("PAO");
  TEST_ASSERT_NOT_NULL(by_code);
  TEST_ASSERT_EQUAL_STRING("Paoli/Thorndale", by_code->display_name);

  const RailLine* by_name = findRailLineByName("Paoli/Thorndale");
  TEST_ASSERT_NOT_NULL(by_name);
  TEST_ASSERT_EQUAL_STRING("PAO", by_name->code);

  // Case-insensitive on both spellings.
  TEST_ASSERT_NOT_NULL(findRailLineByName("pao"));
  TEST_ASSERT_NOT_NULL(findRailLineByName("fox chase"));
  TEST_ASSERT_EQUAL_STRING("TRE", findRailLineByName("Trenton")->code);

  TEST_ASSERT_NULL(findRailLineByName("NOPE"));
  TEST_ASSERT_NULL(findRailLineByName(""));
  TEST_ASSERT_NULL(findRailLineByName("PAO "));  // not trimmed here: the caller owns that
}

// --- Raw-body schedule scan (firstUpcomingScheduleTime) --------------------------------------
//
// The transport layer's service-day check. It reads the SAME field parseBusSchedules() does, off
// the same bytes, without building a document or copying the body - so these tests pin it to the
// parser's own answer rather than to a hand-written expectation.

void test_first_upcoming_schedule_time_matches_the_parser() {
  auto body = transit_test::readFixture("busschedules_21332.json");
  ParseResult<SchedEntry> r = parseBusSchedules(body.data(), body.size());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.items.size() >= 2);

  Epoch earliest = 0;
  for (const SchedEntry& e : r.items) {
    if (earliest == 0 || e.scheduled < earliest) earliest = e.scheduled;
  }
  // "now" well before every entry in the fixture: the scan must find the same first trip.
  TEST_ASSERT_EQUAL_INT64(earliest, firstUpcomingScheduleTime(body.data(), body.size(), earliest - 3600));
}

void test_first_upcoming_schedule_time_skips_entries_already_past() {
  auto body = transit_test::readFixture("busschedules_21332.json");
  ParseResult<SchedEntry> r = parseBusSchedules(body.data(), body.size());
  TEST_ASSERT_TRUE(r.items.size() >= 2);
  std::vector<Epoch> times;
  for (const SchedEntry& e : r.items) times.push_back(e.scheduled);
  std::sort(times.begin(), times.end());

  // A minute past the first entry (beyond the 60 s grace) leaves the second as "first upcoming".
  Epoch now = times[0] + 61;
  TEST_ASSERT_EQUAL_INT64(times[1], firstUpcomingScheduleTime(body.data(), body.size(), now));
  // Within the grace, the first entry still counts - the same rule fetchPlausibleSchedule() uses.
  TEST_ASSERT_EQUAL_INT64(times[0], firstUpcomingScheduleTime(body.data(), body.size(), times[0] + 30));
  // Past everything: nothing upcoming.
  TEST_ASSERT_EQUAL_INT64(0, firstUpcomingScheduleTime(body.data(), body.size(), times.back() + 3600));
}

void test_first_upcoming_schedule_time_reads_the_wrong_service_day_fixture() {
  // The fixture the sticky-cookie steering exists for (NOTES.md 9): a backend answering with
  // tomorrow's first owl trips. Scanned against the day before, the first one is what comes back.
  auto body = transit_test::readFixture("busschedules_21297_wrong_day.json");
  bool ok = false;
  Epoch expected = parseBusScheduleTime("09/15/26 12:32 am", &ok);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT64(expected, firstUpcomingScheduleTime(body.data(), body.size(), expected - 86400));
}

void test_first_upcoming_schedule_time_tolerates_junk_and_truncation() {
  TEST_ASSERT_EQUAL_INT64(0, firstUpcomingScheduleTime(nullptr, 0, 0));
  const char* empty = "";
  TEST_ASSERT_EQUAL_INT64(0, firstUpcomingScheduleTime(reinterpret_cast<const uint8_t*>(empty), 0, 0));
  // A body cut off in the middle of the value: no closing quote, so nothing is parsed and nothing
  // reads past the end.
  const char* cut = "{\"17\":[{\"DateCalender\":\"09\\/13\\/26 10:2";
  TEST_ASSERT_EQUAL_INT64(0, firstUpcomingScheduleTime(reinterpret_cast<const uint8_t*>(cut), strlen(cut), 0));
  // The key without the JSON around it, and a value that is not a date.
  const char* junk = "\"DateCalender\":\"not a date\"";
  TEST_ASSERT_EQUAL_INT64(0, firstUpcomingScheduleTime(reinterpret_cast<const uint8_t*>(junk), strlen(junk), 0));
  // SEPTA's error body carries no DateCalender at all.
  auto err = transit_test::readFixture("busschedules_error_400.json");
  TEST_ASSERT_EQUAL_INT64(0, firstUpcomingScheduleTime(err.data(), err.size(), 0));
}

// --- The TransitView trip filter (0.3.2-rc3) ---------------------------------------------------
//
// mergeStop() reaches a TvVehicle only through findTvByTrip(tv, u.trip_id), for a `u` drawn from
// the retained GTFS-RT updates. So a vehicle whose trip id is not in those updates is never read,
// and building it is nine std::strings spent on nothing. These pin the three behaviours that
// makes safe: what it keeps, what a rejection is NOT, and how it interacts with the cap.

void test_parse_transitview_filter_keeps_only_the_wanted_trips() {
  auto body = transit_test::readFixture("transitview_17.json");
  // The fixture carries two vehicles, trips 3667 (Southbound) and 3585 (Northbound).
  ParseResult<TvVehicle> all = parseTransitView(body.data(), body.size());
  TEST_ASSERT_TRUE(all.ok);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(all.items.size()));

  std::vector<std::string> wanted{"3667"};
  ParseResult<TvVehicle> r;
  parseTransitViewAppend(&r, body.data(), body.size(), TvFilter(&tripIsListed, &wanted));
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(r.items.size()));
  TEST_ASSERT_EQUAL_STRING("3667", r.items[0].trip.c_str());
  // And the vehicle that IS kept is built whole - the filter decides membership, never content.
  TEST_ASSERT_EQUAL_STRING("7477", r.items[0].vehicle_id.c_str());
  TEST_ASSERT_EQUAL_INT(13, r.items[0].late);
  TEST_ASSERT_EQUAL_STRING("20th-Johnston", r.items[0].destination.c_str());
  TEST_ASSERT_EQUAL_STRING("EMPTY", r.items[0].seats.c_str());
  TEST_ASSERT_EQUAL_INT64(1789352300, r.items[0].timestamp);
}

void test_parse_transitview_default_filter_keeps_every_vehicle() {
  // The default is what refreshRouteLiveness() and every other consumer of this library gets, and
  // it must behave exactly as the parse did before the filter existed.
  auto body = transit_test::readFixture("transitview_17.json");
  ParseResult<TvVehicle> r;
  parseTransitViewAppend(&r, body.data(), body.size(), TvFilter());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(r.items.size()));
  TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
}

void test_parse_transitview_a_filtered_out_vehicle_is_not_a_drop() {
  // THE DISTINCTION `tv_dropped` DEPENDS ON. `dropped` means "this config wanted it and the cap
  // had no room" - the reading that says kMaxTvVehicles is too small. A vehicle no configured stop
  // can join to was never wanted, so counting it would make `tv_dropped` report a full cap on
  // every rush-hour route and mean nothing.
  auto body = transit_test::readFixture("transitview_17.json");
  ParseResult<TvVehicle> r = parseTvString(
      std::string(reinterpret_cast<const char*>(body.data()), body.size()),
      TvFilter(&tripIsNeverWanted, nullptr));
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(r.items.size()));
  TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
}

void test_parse_transitview_the_cap_counts_only_vehicles_the_filter_wanted() {
  // 40 vehicles on the wire; the filter wants the 20 with even trip ids. 16 of those fit
  // kMaxTvVehicles, so four are dropped - and the 20 the filter rejected are not in that number.
  std::string body = "{\"bus\":[";
  std::vector<std::string> wanted;
  for (size_t i = 0; i < 40; ++i) {
    if (i) body += ",";
    body += "{\"trip\":\"" + std::to_string(i) + "\",\"VehicleID\":\"v\",\"late\":0}";
    if (i % 2 == 0) wanted.push_back(std::to_string(i));
  }
  body += "]}";
  TEST_ASSERT_EQUAL_UINT32(20, static_cast<uint32_t>(wanted.size()));

  ParseResult<TvVehicle> r = parseTvString(body, TvFilter(&tripIsListed, &wanted));
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kMaxTvVehicles),
                            static_cast<uint32_t>(r.items.size()));
  TEST_ASSERT_EQUAL_UINT32(20 - static_cast<uint32_t>(kMaxTvVehicles), r.dropped);
  // Kept in wire order, and every survivor is one the filter asked for.
  for (size_t i = 0; i < r.items.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING(std::to_string(i * 2).c_str(), r.items[i].trip.c_str());
  }
}

// The resident list is 16 slots since 0.3.2-rc3 and the reason is the filter, so pin the number
// itself: an edit that raises it is an edit to the resting floor and should have to say so here.
void test_transitview_cap_is_sixteen_slots() {
  TEST_ASSERT_EQUAL_UINT32(16, static_cast<uint32_t>(kMaxTvVehicles));
}
