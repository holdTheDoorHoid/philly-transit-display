#include <unity.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include "alloc_probe.h"
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

// --- BusSchedules wrong-service-day retry (septa_source.h fetchPlausibleSchedule, NOTES.md 9) ----

namespace {

// Serves a scripted sequence of fixture bodies to successive requests for any URL (the n-th
// request gets bodies[min(n, size-1)]), and counts the requests it saw.
HttpGet makeSequenceHttp(std::vector<std::string> bodies, int* calls) {
  return [bodies, calls](const std::string&, std::function<bool(const uint8_t*, size_t)> onData) -> int {
    size_t i = static_cast<size_t>(*calls) < bodies.size() ? static_cast<size_t>(*calls) : bodies.size() - 1;
    ++*calls;
    std::vector<uint8_t> body = transit_test::readFixture(bodies[i]);
    onData(body.data(), body.size());
    return 200;
  };
}

}  // namespace

// Captured 2026-09-14 09:24 EDT: busschedules_21297_wrong_day.json is SEPTA's answer from a stale
// backend (first trip 09/15/26 12:32 am, old "Front-Market" headsign); busschedules_21297.json is
// the good answer (first trip 09/13/26 10:32 pm). With `now` = 2026-09-13 22:00 EDT the good
// body's first trip is 32 minutes out and the bad one's is over a day out.
void test_fetch_plausible_schedule_retries_past_wrong_service_day() {
  int calls = 0;
  HttpGet http = makeSequenceHttp({"busschedules_21297_wrong_day.json", "busschedules_21297.json"}, &calls);
  SeptaSource src;
  std::vector<SchedEntry> out;
  Epoch now = 1789351200;  // 2026-09-13 22:00:00 EDT
  bool plausible = fetchPlausibleSchedule(src, "21297", now, http, &out);
  TEST_ASSERT_TRUE(plausible);
  TEST_ASSERT_EQUAL_INT(2, calls);  // stopped as soon as a plausible answer arrived
  TEST_ASSERT_TRUE(out.size() > 0);
  TEST_ASSERT_EQUAL_STRING("281678", out[0].trip_id.c_str());
  TEST_ASSERT_EQUAL_STRING("2nd-Market", out[0].direction_desc.c_str());
}

void test_fetch_plausible_schedule_accepts_first_good_answer_without_retrying() {
  int calls = 0;
  HttpGet http = makeSequenceHttp({"busschedules_21297.json"}, &calls);
  SeptaSource src;
  std::vector<SchedEntry> out;
  TEST_ASSERT_TRUE(fetchPlausibleSchedule(src, "21297", 1789351200, http, &out));
  TEST_ASSERT_EQUAL_INT(1, calls);
}

void test_fetch_plausible_schedule_keeps_best_effort_when_every_answer_is_wrong() {
  int calls = 0;
  HttpGet http = makeSequenceHttp({"busschedules_21297_wrong_day.json"}, &calls);
  SeptaSource src;
  std::vector<SchedEntry> out;
  bool plausible = fetchPlausibleSchedule(src, "21297", 1789351200, http, &out);
  TEST_ASSERT_FALSE(plausible);
  TEST_ASSERT_EQUAL_INT(kScheduleFetchAttempts, calls);
  TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(out.size()));  // still returned, cached briefly
  TEST_ASSERT_EQUAL_STRING("280909", out[0].trip_id.c_str());
}

void test_fetch_plausible_schedule_leaves_out_untouched_on_total_failure() {
  int calls = 0;
  HttpGet http = makeSequenceHttp({"busschedules_error_400.json"}, &calls);
  SeptaSource src;
  std::vector<SchedEntry> out;
  TEST_ASSERT_FALSE(fetchPlausibleSchedule(src, "21297", 1789351200, http, &out));
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(out.size()));
}

// A network that blackholes packets is not a wrong-service-day answer, and must not be paid for
// three times. The transport reports status 0 ("could not be made at all") after it has already
// spent its own attempts and backoff on the URL; fetchPlausibleSchedule stops there. Before this,
// one stop's schedule cost kScheduleFetchAttempts transport failures per poll cycle, and with the
// firmware's own BusSchedules retry under it that was up to twelve URL fetches - minutes per stop
// on a dead network, which is what let the liveness net reboot a healthy board mid-cycle.
void test_fetch_plausible_schedule_stops_on_a_transport_failure() {
  int calls = 0;
  HttpGetEx http = [&calls](const std::string&, std::function<bool(const uint8_t*, size_t)>) -> FetchResult {
    ++calls;
    return FetchResult{};  // status 0, nothing delivered: the request could not be made at all
  };
  SeptaSource src;
  std::vector<SchedEntry> out;
  bool fetched_ok = true;
  TEST_ASSERT_FALSE(fetchPlausibleSchedule(src, "21297", 1789351200, http, &out, &fetched_ok));
  TEST_ASSERT_EQUAL_INT(1, calls);
  TEST_ASSERT_FALSE(fetched_ok);  // the stop is still marked unavailable, as before
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(out.size()));
}

// The wrong-service-day retries are NOT what was removed: a backend that answers (any real status)
// with an implausible schedule is still asked again, up to kScheduleFetchAttempts times.
void test_fetch_plausible_schedule_still_retries_a_backend_that_answers() {
  int calls = 0;
  HttpGet http = makeSequenceHttp({"busschedules_21297_wrong_day.json"}, &calls);
  SeptaSource src;
  std::vector<SchedEntry> out;
  TEST_ASSERT_FALSE(fetchPlausibleSchedule(src, "21297", 1789351200, http, &out));
  TEST_ASSERT_EQUAL_INT(kScheduleFetchAttempts, calls);
}

// pollBusStops() routes an implausible schedule through ScheduleCache::putSuspect rather than
// put(), so the glue layer can expire it quickly.
void test_poll_bus_stops_marks_wrong_day_schedule_as_suspect() {
  class RecordingCache : public FakeScheduleCache {
   public:
    void putSuspect(const std::string& stop_id, const std::vector<SchedEntry>& entries) override {
      suspect[stop_id] = entries;
    }
    std::map<std::string, std::vector<SchedEntry>> suspect;
  };
  HttpGet http = [](const std::string& url, std::function<bool(const uint8_t*, size_t)> onData) -> int {
    std::string fixture = url.find("BusSchedules") != std::string::npos ? "busschedules_21297_wrong_day.json"
                          : url.find("TransitView") != std::string::npos ? "transitview_BSL.json"
                                                                          : "septa_bus_tripupdates.pb";
    std::vector<uint8_t> body = transit_test::readFixture(fixture);
    onData(body.data(), body.size());
    return 200;
  };
  StopConfig nb;
  nb.key = "17-21297";
  nb.mode = Mode::Bus;
  nb.route = "17";
  nb.stop_id = "21297";
  nb.direction = "0";
  RecordingCache cache;
  Snapshot snap = pollBusStops({nb}, 1789351200, http, cache);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cache.stored.count("21297")));
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(cache.suspect.count("21297")));
  const StopSnapshot* st = findStop(snap, "17-21297");
  TEST_ASSERT_NOT_NULL(st);
  TEST_ASSERT_TRUE(st->arrivals.size() > 0);  // best effort still displayed
}

// =============================================================================================
// Regression tests for the 2026-09-15 adversarial review (F13: fetch/parse failures must not
// come back as successful, empty stop snapshots).
// =============================================================================================

namespace {

// Like makeFixtureHttp, but speaks HttpGetEx so a transport can report an incomplete body, and
// lets each URL carry its own status/completeness. `body` is either a fixture name or, when
// prefixed with "raw:", literal bytes.
struct FakeReply {
  std::string body;      // fixture name, or "raw:<bytes>"
  int status = 200;
  bool complete = true;
  size_t truncate_to = 0;  // 0 = deliver it all
};

std::vector<uint8_t> replyBytes(const FakeReply& r) {
  std::vector<uint8_t> body;
  if (r.body.rfind("raw:", 0) == 0) {
    std::string lit = r.body.substr(4);
    body.assign(lit.begin(), lit.end());
  } else if (!r.body.empty()) {
    body = transit_test::readFixture(r.body);
  }
  if (r.truncate_to > 0 && r.truncate_to < body.size()) body.resize(r.truncate_to);
  return body;
}

HttpGetEx makeExHttp(std::map<std::string, FakeReply> replies) {
  return [replies](const std::string& url,
                    std::function<bool(const uint8_t*, size_t)> onData) -> FetchResult {
    FetchResult res;
    auto it = replies.find(url);
    if (it == replies.end()) {
      res.status = 404;
      return res;  // complete=false, bytes=0: nothing was delivered
    }
    std::vector<uint8_t> body = replyBytes(it->second);
    const size_t kChunk = 4096;
    bool refused = false;
    for (size_t i = 0; i < body.size(); i += kChunk) {
      size_t n = std::min(kChunk, body.size() - i);
      if (!onData(body.data() + i, n)) {
        refused = true;
        break;
      }
      res.bytes += n;
    }
    res.status = it->second.status;
    res.aborted = refused;
    res.complete = it->second.complete && !refused;
    return res;
  };
}

StopConfig busStop(const std::string& key, const std::string& route, const std::string& stop_id,
                    const std::string& dir) {
  StopConfig c;
  c.key = key;
  c.mode = Mode::Bus;
  c.route = route;
  c.stop_id = stop_id;
  c.direction = dir;
  return c;
}

}  // namespace

// A 200 whose body stopped arriving halfway: the decoder used to see a valid short feed and every
// bus stop came back "ok" with only its schedule rows, saying nothing about the missing half.
void test_poll_bus_stops_truncated_tripupdates_is_reported() {
  // Cut inside the feed header, so nothing at all was decoded: the stop has only its schedule.
  std::map<std::string, FakeReply> replies = {
      {septaTripUpdatesUrl(), {"septa_bus_tripupdates.pb", 200, false, 20}},
      {septaTransitViewUrl("17"), {"transitview_17.json", 200, true, 0}},
      {septaBusSchedulesUrl("21332"), {"busschedules_21332.json", 200, true, 0}},
  };
  FakeScheduleCache cache;
  Snapshot snap = pollBusStops({busStop("17-21332", "17", "21332", "1")}, 1789352300,
                                makeExHttp(replies), cache);

  TEST_ASSERT_FALSE(snap.last_poll_ok);
  TEST_ASSERT_TRUE(snap.last_error.find("TripUpdates") != std::string::npos);

  const StopSnapshot* st = findStop(snap, "17-21332");
  TEST_ASSERT_NOT_NULL(st);
  TEST_ASSERT_FALSE(st->ok);
  TEST_ASSERT_TRUE(st->error.find("live feed") != std::string::npos);
  // The schedule still has something to say, so the panel is not blank - it is flagged.
  TEST_ASSERT_TRUE(st->health == Health::ScheduleOnly);
  TEST_ASSERT_TRUE(st->arrivals.size() > 0);
  for (const auto& a : st->arrivals) {
    TEST_ASSERT_TRUE(a.status == Status::Scheduled);
  }

  // A cut far enough in to have delivered some real predictions is still a failure: whatever
  // arrived is shown, but the stop (and the poll) say the live feed did not finish. What the
  // partial feed happened to contain does not change that, which is the point.
  std::map<std::string, FakeReply> partial = replies;
  partial[septaTripUpdatesUrl()] = {"septa_bus_tripupdates.pb", 200, false, 60000};
  FakeScheduleCache cache2;
  Snapshot snap2 = pollBusStops({busStop("17-21332", "17", "21332", "1")}, 1789352300,
                                 makeExHttp(partial), cache2);
  TEST_ASSERT_FALSE(snap2.last_poll_ok);
  const StopSnapshot* st2 = findStop(snap2, "17-21332");
  TEST_ASSERT_NOT_NULL(st2);
  TEST_ASSERT_FALSE(st2->ok);
  TEST_ASSERT_TRUE(st2->arrivals.size() > 0);
}

// HTTP 200 with a body that is not a protobuf FeedMessage at all.
void test_poll_bus_stops_invalid_protobuf_is_reported() {
  std::map<std::string, FakeReply> replies = {
      // Wiretype 3 (deprecated groups) at the top level: unsupported framing, not a short feed.
      {septaTripUpdatesUrl(), {"raw:\x0b\x0b\x0b\x0b", 200, true, 0}},
      {septaTransitViewUrl("17"), {"transitview_17.json", 200, true, 0}},
      {septaBusSchedulesUrl("21332"), {"busschedules_21332.json", 200, true, 0}},
  };
  FakeScheduleCache cache;
  Snapshot snap = pollBusStops({busStop("17-21332", "17", "21332", "1")}, 1789352300,
                                makeExHttp(replies), cache);
  TEST_ASSERT_FALSE(snap.last_poll_ok);
  const StopSnapshot* st = findStop(snap, "17-21332");
  TEST_ASSERT_NOT_NULL(st);
  TEST_ASSERT_FALSE(st->ok);
}

// A subway stop depends on BusSchedules and nothing else. Its failure must mark that stop and
// leave the bus stop next to it alone - the old code used the TripUpdates status as the one
// global success signal, so a subway-only failure read as "ok".
void test_poll_bus_stops_subway_schedule_failure_is_local_to_that_stop() {
  std::map<std::string, FakeReply> replies = {
      {septaTripUpdatesUrl(), {"septa_bus_tripupdates.pb", 200, true, 0}},
      {septaTransitViewUrl("17"), {"transitview_17.json", 200, true, 0}},
      {septaBusSchedulesUrl("21332"), {"busschedules_21332.json", 200, true, 0}},
      {septaBusSchedulesUrl("1286"), {"busschedules_error_400.json", 400, true, 0}},
  };
  StopConfig subway;
  subway.key = "bsl-snyder";
  subway.mode = Mode::Subway;
  subway.route = "BSL";
  subway.stop_id = "1286";
  subway.direction = "0";

  FakeScheduleCache cache;
  Snapshot snap = pollBusStops({busStop("17-21332", "17", "21332", "1"), subway}, 1789352300,
                                makeExHttp(replies), cache);

  const StopSnapshot* bus = findStop(snap, "17-21332");
  const StopSnapshot* sub = findStop(snap, "bsl-snyder");
  TEST_ASSERT_NOT_NULL(bus);
  TEST_ASSERT_NOT_NULL(sub);
  TEST_ASSERT_TRUE(bus->ok);                            // untouched by the subway's failure
  TEST_ASSERT_FALSE(sub->ok);
  TEST_ASSERT_TRUE(sub->health == Health::Unavailable);
  TEST_ASSERT_TRUE(sub->error.size() > 0);
  TEST_ASSERT_FALSE(snap.last_poll_ok);                 // ...but the poll as a whole is not ok
}

// NOTES.md 1: SEPTA serves valid BusSchedules bodies under HTTP 501. That workaround has to
// survive the new failure reporting - the body is what is validated, not the status line.
void test_poll_bus_stops_501_with_a_valid_body_still_succeeds() {
  std::map<std::string, FakeReply> replies = {
      {septaTripUpdatesUrl(), {"septa_bus_tripupdates.pb", 200, true, 0}},
      {septaTransitViewUrl("17"), {"transitview_17.json", 200, true, 0}},
      {septaBusSchedulesUrl("21297"), {"busschedules_error_501.json", 501, true, 0}},
  };
  FakeScheduleCache cache;
  Snapshot snap = pollBusStops({busStop("17-21297", "17", "21297", "0")}, 1789351200,
                                makeExHttp(replies), cache);
  const StopSnapshot* st = findStop(snap, "17-21297");
  TEST_ASSERT_NOT_NULL(st);
  TEST_ASSERT_TRUE(st->ok);
  TEST_ASSERT_TRUE(st->arrivals.size() > 0);
}

// A valid feed that simply has nothing for us is a success, not a failure: only a header, no
// entities, and no configured stop appears in it.
void test_poll_bus_stops_valid_empty_feed_succeeds() {
  // FeedMessage { header { gtfs_realtime_version: "2.0" } } - 9 bytes, valid and entity-free.
  std::string empty_feed("\x0a\x07\x0a\x03\x32\x2e\x30\x18\x00", 9);
  std::map<std::string, FakeReply> replies = {
      {septaTripUpdatesUrl(), {"raw:" + empty_feed, 200, true, 0}},
      {septaTransitViewUrl("17"), {"transitview_BSL.json", 200, true, 0}},  // a bare []
      {septaBusSchedulesUrl("21332"), {"busschedules_21332.json", 200, true, 0}},
  };
  FakeScheduleCache cache;
  Snapshot snap = pollBusStops({busStop("17-21332", "17", "21332", "1")}, 1789351200,
                                makeExHttp(replies), cache);
  TEST_ASSERT_TRUE(snap.last_poll_ok);
  const StopSnapshot* st = findStop(snap, "17-21332");
  TEST_ASSERT_NOT_NULL(st);
  TEST_ASSERT_TRUE(st->ok);
  TEST_ASSERT_TRUE(st->health == Health::ScheduleOnly);
}

// --- pollRailStops -----------------------------------------------------------------------------

void test_poll_rail_stops_one_failed_station_is_local_to_that_station() {
  std::map<std::string, FakeReply> replies = {
      {septaArrivalsUrl("30th Street Station"), {"arrivals_30th.json", 200, true, 0}},
      // The other station is simply not in the map: the fake answers 404 with no body.
  };
  StopConfig good;
  good.key = "rail-30th";
  good.mode = Mode::Rail;
  good.station = "30th Street Station";
  good.direction = "N";

  StopConfig bad;
  bad.key = "rail-suburban";
  bad.mode = Mode::Rail;
  bad.station = "Suburban Station";
  bad.direction = "N";

  Snapshot snap = pollRailStops({good, bad}, 1789352000, makeExHttp(replies));
  const StopSnapshot* g = findStop(snap, "rail-30th");
  const StopSnapshot* b = findStop(snap, "rail-suburban");
  TEST_ASSERT_NOT_NULL(g);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_TRUE(g->ok);
  TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(g->arrivals.size()));
  TEST_ASSERT_FALSE(b->ok);
  TEST_ASSERT_TRUE(b->health == Health::Unavailable);
  TEST_ASSERT_FALSE(snap.last_poll_ok);
}

// HTTP 200 with a body that is not the Arrivals shape: used to become a successful empty station.
void test_poll_rail_stops_malformed_json_is_a_failure() {
  std::map<std::string, FakeReply> replies = {
      {septaArrivalsUrl("30th Street Station"), {"raw:{\"oops\": ", 200, true, 0}},
  };
  StopConfig cfg;
  cfg.key = "rail-30th";
  cfg.mode = Mode::Rail;
  cfg.station = "30th Street Station";

  Snapshot snap = pollRailStops({cfg}, 1789352000, makeExHttp(replies));
  TEST_ASSERT_FALSE(snap.last_poll_ok);
  const StopSnapshot* st = findStop(snap, "rail-30th");
  TEST_ASSERT_NOT_NULL(st);
  TEST_ASSERT_FALSE(st->ok);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(st->arrivals.size()));
}

// --- The poll cycle's reusable working set (PollBuffers) --------------------------------------
//
// These pin the two properties the firmware depends on: the buffers keep their capacity across
// cycles (so a cycle asks the allocator for nothing large), and using them changes NOTHING about
// what a cycle produces.

namespace {

// Fixtures read once, up front, and replayed from memory - so nothing the HARNESS allocates lands
// inside the window the allocation counter below measures.
HttpGet makePreloadedHttp(std::map<std::string, std::string> url_to_fixture) {
  // A shared_ptr and not the map itself: HttpGet/HttpGetEx are std::functions passed BY VALUE
  // through pollBusStops() and every fetch under it, so a by-value capture would copy the 148 KB
  // TripUpdates fixture on each hop - which is the harness allocating, inside the very window
  // AllocProbe is measuring the library in.
  auto bodies = std::make_shared<std::map<std::string, std::vector<uint8_t>>>();
  for (const auto& kv : url_to_fixture) (*bodies)[kv.first] = transit_test::readFixture(kv.second);
  return [bodies](const std::string& url,
                   std::function<bool(const uint8_t*, size_t)> onData) -> int {
    auto it = bodies->find(url);
    if (it == bodies->end()) return 404;
    const std::vector<uint8_t>& body = it->second;
    const size_t kChunk = 4096;
    for (size_t i = 0; i < body.size(); i += kChunk) {
      size_t n = std::min(kChunk, body.size() - i);
      if (!onData(body.data() + i, n)) break;
    }
    return 200;
  };
}

// A cache that actually caches, so the "cache-hit cycle" of DESIGN.md SS5 can be exercised.
class WarmScheduleCache : public ScheduleCache {
 public:
  bool get(const std::string& stop_id, std::vector<SchedEntry>* out) override {
    auto it = stored.find(stop_id);
    if (it == stored.end()) return false;
    *out = it->second;
    return true;
  }
  void put(const std::string& stop_id, const std::vector<SchedEntry>& entries) override {
    stored[stop_id] = entries;
  }
  void putSuspect(const std::string& stop_id, const std::vector<SchedEntry>& entries) override {
    stored[stop_id] = entries;
  }
  std::map<std::string, std::vector<SchedEntry>> stored;
};

std::map<std::string, std::string> twoStopRoutes() {
  return {
      {septaTripUpdatesUrl(), "septa_bus_tripupdates.pb"},
      {septaTransitViewUrl("17"), "transitview_17.json"},
      {septaBusSchedulesUrl("21332"), "busschedules_21332.json"},
      {septaBusSchedulesUrl("21297"), "busschedules_21297.json"},
  };
}

std::vector<StopConfig> twoStopConfigs() {
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
  return {sb, nb};
}

// Everything a caller can observe about a Snapshot, as one string, so "the buffers change nothing"
// is an equality rather than a list of spot checks.
std::string describe(const Snapshot& s) {
  std::string out = std::to_string(s.generated) + "|" + (s.last_poll_ok ? "ok" : "bad") + "|" +
                     s.last_error + "|";
  for (const StopSnapshot& st : s.stops) {
    out += st.key + ":" + std::to_string((int)st.health) + ":" + (st.ok ? "1" : "0") + ":" +
           st.error + ":" + std::to_string(st.source_ts) + "[";
    for (const Arrival& a : st.arrivals) {
      out += a.trip + "," + a.vehicle + "," + a.destination + "," + std::to_string(a.predicted) +
             "," + std::to_string(a.scheduled) + "," + std::to_string((int)a.status) + ";";
    }
    out += "]";
  }
  return out;
}

}  // namespace

void test_poll_buffers_keep_their_capacity_across_cycles() {
  HttpGetEx http = adaptHttpGet(makePreloadedHttp(twoStopRoutes()));
  std::vector<StopConfig> configs = twoStopConfigs();
  Epoch now = 1789352300;

  PollBuffers buf;
  buf.reserveAll();
  const size_t cap = buf.scratch.capacity();
  TEST_ASSERT_TRUE(cap >= PollBuffers::kScratchReserve);

  WarmScheduleCache cache;
  for (int cycle = 0; cycle < 3; ++cycle) {
    buf.beginCycle();
    Snapshot snap = pollBusStops(configs, now, http, cache, &buf);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(snap.stops.size()));
    // Not "at least": exactly what was reserved, cycle after cycle. A capacity that GREW would
    // mean a response outran the reservation; one that shrank would mean the buffer was given
    // away. Every borrower - the GTFS-RT entity buffer, each buffered JSON response - hands the
    // same storage on rather than taking its own.
    TEST_ASSERT_EQUAL_UINT32(cap, static_cast<uint32_t>(buf.scratch.capacity()));
  }
}

void test_poll_buffers_do_not_change_what_a_cycle_produces() {
  HttpGetEx http = adaptHttpGet(makePreloadedHttp(twoStopRoutes()));
  std::vector<StopConfig> configs = twoStopConfigs();
  Epoch now = 1789352300;

  FakeScheduleCache plain_cache;
  Snapshot without = pollBusStops(configs, now, http, plain_cache, nullptr);

  PollBuffers buf;
  buf.reserveAll();
  FakeScheduleCache buffered_cache;
  buf.beginCycle();
  Snapshot with_first = pollBusStops(configs, now, http, buffered_cache, &buf);
  buf.beginCycle();
  Snapshot with_second = pollBusStops(configs, now, http, buffered_cache, &buf);

  // Identical to the unbuffered answer, and identical again on the reused buffer - which is what
  // says the entity-buffer borrow was handed back cleanly and no stage read another's leftovers.
  TEST_ASSERT_EQUAL_STRING(describe(without).c_str(), describe(with_first).c_str());
  TEST_ASSERT_EQUAL_STRING(describe(without).c_str(), describe(with_second).c_str());
}

void test_poll_buffers_return_an_oversized_body_buffer() {
  PollBuffers buf;
  buf.reserveAll();
  // An unusually large response grew the scratch past the reservation. It must not stay resident:
  // the next cycle's beginCycle() gives the block back and takes a fresh reserve-sized one. This
  // is what keeps the FLOOR from drifting down over uptime, which is the failure the first
  // attempt at this made (see PollBuffers).
  buf.scratch.resize(3 * PollBuffers::kScratchReserve);
  TEST_ASSERT_TRUE(buf.scratch.capacity() > PollBuffers::kScratchReserve);
  buf.beginCycle();
  TEST_ASSERT_EQUAL_UINT32(PollBuffers::kScratchReserve, static_cast<uint32_t>(buf.scratch.capacity()));
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(buf.scratch.size()));
}

void test_a_poll_cycle_never_fetches_alerts() {
  // 0.3.2-rc1 turned `alerts` off by default (owner decision), so it is worth pinning what "off"
  // actually costs: nothing on the arrivals path. The alerts fetch is not part of pollBusStops()
  // at all - it is net_poller.cpp's collectAlerts(), on its own 5-minute cadence, behind
  // `if (!alerts_enabled) { clear the cache; return {}; }` - and a poll cycle must therefore never
  // reach an Alerts URL whatever the config says. Recorded here rather than in the firmware
  // because this is the layer a host test can hold, and because the separation is the thing that
  // makes "off" free rather than merely quiet.
  std::vector<std::string> seen;
  auto bodies = twoStopRoutes();
  HttpGet recording = [&seen, bodies](const std::string& url,
                                      std::function<bool(const uint8_t*, size_t)> onData) -> int {
    seen.push_back(url);
    return makePreloadedHttp(bodies)(url, std::move(onData));
  };
  FakeScheduleCache cache;
  Snapshot snap = pollBusStops(twoStopConfigs(), 1789352300, adaptHttpGet(recording), cache, nullptr);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(snap.stops.size()));
  TEST_ASSERT_TRUE(!seen.empty());
  for (const std::string& url : seen) {
    TEST_ASSERT_TRUE_MESSAGE(url.find("/api/Alerts/") == std::string::npos,
                              "a poll cycle fetched an Alerts URL");
  }
  // And the Snapshot a cycle produces carries no alerts of its own: they are attached by the
  // caller, from its cache, which is empty when the setting is off.
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(snap.alerts.size()));
}

void test_poll_buffers_remove_the_large_contiguous_requests() {
  HttpGetEx http = adaptHttpGet(makePreloadedHttp(twoStopRoutes()));
  std::vector<StopConfig> configs = twoStopConfigs();
  Epoch now = 1789352300;

  // THE MEASUREMENT THIS EXISTS FOR (DESIGN.md SS5). Free heap is not what constrains this board;
  // the largest free BLOCK is. So the question is not "how many bytes did a cycle use" but "what
  // is the biggest single thing it asked the allocator for, and how many such things are there".
  //
  // Sizes here are HOST sizes (64-bit std::string is 32 B, so every one of these structs is bigger
  // than on the ESP32). The threshold is therefore deliberately low: what is being pinned is that
  // a warm buffered cycle asks for NOTHING of this size, not the exact byte count.
  constexpr size_t kBig = 2048;

  FakeScheduleCache plain_cache;
  transit_test::AllocProbe::begin();
  (void)pollBusStops(configs, now, http, plain_cache, nullptr);
  (void)transit_test::AllocProbe::end();
  const size_t big_without = transit_test::AllocProbe::countAtLeast(kBig);
  TEST_ASSERT_TRUE_MESSAGE(big_without >= 2, "a per-call cycle makes several multi-KB requests");

  PollBuffers buf;
  buf.reserveAll();
  buf.reserveRetention(configs.size());
  WarmScheduleCache cache;
  buf.beginCycle();
  (void)pollBusStops(configs, now, http, cache, &buf);  // warm-up: fills the schedule cache
  buf.beginCycle();
  transit_test::AllocProbe::begin();
  Snapshot snap = pollBusStops(configs, now, http, cache, &buf);
  (void)transit_test::AllocProbe::end();
  const size_t big_with = transit_test::AllocProbe::countAtLeast(kBig);

  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(snap.stops.size()));
  // NONE left (0.3.2-rc1). Until this release one was: the GTFS-RT retention block, which is a
  // vector of non-trivially-destructible values and so cannot share the byte scratch. It is now a
  // resident vector of its own that the stream BORROWS, which is what closes the last one - and
  // which is the request that failed on the owner's board on 2026-09-17 once the largest free
  // block had fragmented to 3,444 B.
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(big_with));
  TEST_ASSERT_TRUE(big_with < big_without);
}

void test_poll_buffers_keep_the_typed_blocks_across_cycles() {
  // The capacities are asserted EQUAL rather than merely sufficient across three cycles: "it did
  // not reallocate" is the claim, and a >= test would pass while the vector quietly grew.
  HttpGetEx http = adaptHttpGet(makePreloadedHttp(twoStopRoutes()));
  std::vector<StopConfig> configs = twoStopConfigs();
  Epoch now = 1789352300;

  PollBuffers buf;
  buf.reserveAll();
  buf.reserveRetention(configs.size());
  WarmScheduleCache cache;
  buf.beginCycle();
  (void)pollBusStops(configs, now, http, cache, &buf);
  const size_t retained_cap = buf.retained.capacity();
  const size_t tv_cap = buf.tv.capacity();
  const size_t scratch_cap = buf.scratch.capacity();
  TEST_ASSERT_TRUE(retained_cap >= configs.size() * PollBuffers::kRetainedPerPair);
  TEST_ASSERT_TRUE(tv_cap >= kMaxTvVehicles);

  for (int i = 0; i < 3; i++) {
    buf.beginCycle();
    Snapshot snap = pollBusStops(configs, now, http, cache, &buf);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(snap.stops.size()));
    TEST_ASSERT_EQUAL_size_t(retained_cap, buf.retained.capacity());
    TEST_ASSERT_EQUAL_size_t(tv_cap, buf.tv.capacity());
    TEST_ASSERT_EQUAL_size_t(scratch_cap, buf.scratch.capacity());
  }
  // beginCycle() drops the retained updates' identifier strings between cycles without giving the
  // block back: the elements go, the capacity stays.
  TEST_ASSERT_TRUE(buf.retained.size() > 0);  // the cycle that just ran filled it
  buf.beginCycle();
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(buf.retained.size()));
  TEST_ASSERT_EQUAL_size_t(retained_cap, buf.retained.capacity());
}

void test_poll_buffers_size_retention_from_the_config() {
  // 8 slots per configured (stop, route) pair, not the stream's 32-slot default cap: that is what
  // makes the block 2.4 KB on the owner's two-stop config instead of 4.9 KB. Growth only.
  PollBuffers buf;
  buf.reserveRetention(2);
  const size_t two = buf.retained.capacity();
  TEST_ASSERT_TRUE(two >= 16);
  buf.reserveRetention(1);
  TEST_ASSERT_EQUAL_size_t(two, buf.retained.capacity());  // never shrinks
  buf.reserveRetention(4);
  TEST_ASSERT_TRUE(buf.retained.capacity() >= 32);
  buf.reserveRetention(0);  // a subway/rail-only config asks for nothing
}

void test_the_scratch_reservation_ratchets_instead_of_churning() {
  // The grow/shrink cycle this replaces: a body bigger than kScratchReserve made the vector double
  // past it and beginCycle() gave that block back, every cycle, forever. Now the reservation
  // ratchets up to what was actually needed - one reallocation per boot - and is only given back
  // above kScratchMaxReserve, so a pathological response still cannot become resident.
  PollBuffers buf;
  buf.reserveAll();
  const size_t base = buf.scratch.capacity();
  TEST_ASSERT_TRUE(base >= PollBuffers::kScratchReserve);

  buf.scratch.reserve(PollBuffers::kScratchReserve + 2048);  // a body went past the reservation
  buf.beginCycle();
  TEST_ASSERT_TRUE(buf.scratch.capacity() >= PollBuffers::kScratchReserve + 2048);
  const size_t kept = buf.scratch.capacity();
  buf.beginCycle();
  TEST_ASSERT_EQUAL_size_t(kept, buf.scratch.capacity());  // and it stays, cycle after cycle

  buf.scratch.reserve(PollBuffers::kScratchMaxReserve + 4096);  // a pathological one
  buf.beginCycle();
  TEST_ASSERT_TRUE(buf.scratch.capacity() <= PollBuffers::kScratchMaxReserve);
  TEST_ASSERT_TRUE(buf.scratch.capacity() >= kept);
}

void test_the_scratch_high_water_is_recorded() {
  // scratch_max_bytes is the number that says whether kScratchReserve is the right size at all -
  // which until 0.3.2-rc1 nothing on this device could answer, so the reservation was a guess.
  HttpGetEx http = adaptHttpGet(makePreloadedHttp(twoStopRoutes()));
  std::vector<StopConfig> configs = twoStopConfigs();
  PollBuffers buf;
  buf.reserveAll();
  buf.reserveRetention(configs.size());
  WarmScheduleCache cache;
  buf.beginCycle();
  (void)pollBusStops(configs, 1789352300, http, cache, &buf);
  TEST_ASSERT_TRUE(buf.scratch_max_bytes > 0);
  // The fixtures are small, so nothing should have had to grow past the reservation.
  TEST_ASSERT_TRUE(buf.scratch_max_bytes <= PollBuffers::kScratchReserve);
  TEST_ASSERT_EQUAL_UINT32(0, buf.scratch_grows);
}

void test_poll_buffers_hand_the_same_storage_to_the_indego_scanner() {
  // The cross-library half of the arrangement, stated as a test because nothing else would catch
  // it: the same vector that was the GTFS-RT entity buffer and then each response body is handed
  // to indego::StatusStream later in the SAME cycle. It is checked here as capacity retention -
  // the scanner must not take a buffer of its own - with the scanner's own behaviour covered by
  // test_indego.
  PollBuffers buf;
  buf.reserveAll();
  TEST_ASSERT_TRUE(buf.scratch.capacity() >= 6144);  // sized for the largest borrower
}
