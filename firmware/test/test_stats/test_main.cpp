// Unity tests for transit_stats: events (CSV), ArrivalTracker (StopSnapshot -> LogEvent) and
// StatsAggregator (LogEvent CSV stream -> DESIGN.md §9.3 JSON). Run with:
//   cd firmware && pio test -e native -f test_stats
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "transit_core/model.h"
#include "transit_core/timeparse.h"
#include "transit_stats/aggregate.h"
#include "transit_stats/events.h"
#include "transit_stats/log_window.h"
#include "transit_stats/overview.h"
#include "transit_stats/summary.h"
#include "transit_stats/tracker.h"

using transit_stats::ArrivalTracker;
using transit_stats::EventType;
using transit_stats::LogEvent;
using transit_stats::OverviewAggregator;
using transit_stats::StatsAggregator;
using transit_stats::StopCounters;
using transit_stats::StopSummary;

void setUp(void) {}
void tearDown(void) {}

// =============================================================================================
// events.h/.cpp: CSV round trip
// =============================================================================================

static void test_csv_round_trip_full(void) {
  LogEvent ev;
  ev.ts = 1234567890;
  ev.event = EventType::Arrive;
  ev.stop_key = "17-21332";
  ev.route = "17";
  ev.dir = "0";
  ev.trip = "3667";
  ev.vehicle = "7477";
  ev.scheduled_ts = 1234567800;
  ev.predicted_ts = 1234567850;
  ev.actual_ts = 1234567860;
  ev.late_min = 13;
  ev.horizon_s = 120;
  ev.headway_s = 900;
  ev.note = "bus operator said, \"running very late, sorry\"";  // commas AND quotes

  const std::string csv = transit_stats::toCsv(ev);
  LogEvent parsed;
  TEST_ASSERT_TRUE(transit_stats::fromCsv(csv.c_str(), csv.size(), parsed));

  TEST_ASSERT_EQUAL_INT64(ev.ts, parsed.ts);
  TEST_ASSERT_TRUE(parsed.event == EventType::Arrive);
  TEST_ASSERT_EQUAL_STRING(ev.stop_key.c_str(), parsed.stop_key.c_str());
  TEST_ASSERT_EQUAL_STRING(ev.route.c_str(), parsed.route.c_str());
  TEST_ASSERT_EQUAL_STRING(ev.dir.c_str(), parsed.dir.c_str());
  TEST_ASSERT_EQUAL_STRING(ev.trip.c_str(), parsed.trip.c_str());
  TEST_ASSERT_EQUAL_STRING(ev.vehicle.c_str(), parsed.vehicle.c_str());
  TEST_ASSERT_TRUE(parsed.scheduled_ts.has_value());
  TEST_ASSERT_EQUAL_INT64(*ev.scheduled_ts, *parsed.scheduled_ts);
  TEST_ASSERT_TRUE(parsed.predicted_ts.has_value());
  TEST_ASSERT_EQUAL_INT64(*ev.predicted_ts, *parsed.predicted_ts);
  TEST_ASSERT_TRUE(parsed.actual_ts.has_value());
  TEST_ASSERT_EQUAL_INT64(*ev.actual_ts, *parsed.actual_ts);
  TEST_ASSERT_TRUE(parsed.late_min.has_value());
  TEST_ASSERT_EQUAL_INT32(*ev.late_min, *parsed.late_min);
  TEST_ASSERT_TRUE(parsed.horizon_s.has_value());
  TEST_ASSERT_EQUAL_INT32(*ev.horizon_s, *parsed.horizon_s);
  TEST_ASSERT_TRUE(parsed.headway_s.has_value());
  TEST_ASSERT_EQUAL_INT32(*ev.headway_s, *parsed.headway_s);
  TEST_ASSERT_EQUAL_STRING(ev.note.c_str(), parsed.note.c_str());

  // Tolerate a trailing CR (CRLF line endings), per fromCsv's documented contract.
  const std::string crlf = csv + "\r";
  LogEvent parsed_crlf;
  TEST_ASSERT_TRUE(transit_stats::fromCsv(crlf.c_str(), crlf.size(), parsed_crlf));
  TEST_ASSERT_EQUAL_STRING(ev.note.c_str(), parsed_crlf.note.c_str());
}

static void test_csv_round_trip_empty_optionals_are_empty_not_zero(void) {
  LogEvent ev;
  ev.ts = 42;
  ev.event = EventType::Ghost;
  ev.stop_key = "S";
  ev.route = "17";
  ev.dir = "0";
  ev.trip = "T1";
  // scheduled_ts / predicted_ts / actual_ts / late_min / horizon_s / headway_s left unset.
  ev.note = "";

  const std::string csv = transit_stats::toCsv(ev);
  // Column order: ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,
  //               late_min,horizon_s,headway_s,note,seats,temp,wx,alert,bikes,ebikes,docks
  TEST_ASSERT_EQUAL_STRING("42,ghost,S,17,0,T1,,,,,,,,,,,,,,,", csv.c_str());

  LogEvent parsed;
  TEST_ASSERT_TRUE(transit_stats::fromCsv(csv.c_str(), csv.size(), parsed));
  TEST_ASSERT_FALSE(parsed.scheduled_ts.has_value());
  TEST_ASSERT_FALSE(parsed.predicted_ts.has_value());
  TEST_ASSERT_FALSE(parsed.actual_ts.has_value());
  TEST_ASSERT_FALSE(parsed.late_min.has_value());
  TEST_ASSERT_FALSE(parsed.horizon_s.has_value());
  TEST_ASSERT_FALSE(parsed.headway_s.has_value());
}

static void test_csv_header_line_is_rejected(void) {
  const char* header = transit_stats::csvHeader();
  LogEvent ev;
  TEST_ASSERT_FALSE(transit_stats::fromCsv(header, std::string(header).size(), ev));
}

// Log schema v2 (DESIGN.md §9.1): a full 21-column row, including every new column, round-trips.
static void test_csv_round_trip_v2_full(void) {
  LogEvent ev;
  ev.ts = 1757900000;
  ev.event = EventType::Arrive;
  ev.stop_key = "17-21332";
  ev.route = "17";
  ev.dir = "0";
  ev.trip = "3667";
  ev.vehicle = "7477";
  ev.actual_ts = 1757900010;
  ev.late_min = 4;
  ev.note = "";
  ev.seats = "standing";
  ev.temp = 71;
  ev.wx = 3;
  ev.alert = 2;
  ev.bikes = 5;
  ev.ebikes = 2;
  ev.docks = 7;

  const std::string csv = transit_stats::toCsv(ev);
  LogEvent parsed;
  TEST_ASSERT_TRUE(transit_stats::fromCsv(csv.c_str(), csv.size(), parsed));

  TEST_ASSERT_EQUAL_STRING("standing", parsed.seats.c_str());
  TEST_ASSERT_TRUE(parsed.temp.has_value());
  TEST_ASSERT_EQUAL_INT32(71, *parsed.temp);
  TEST_ASSERT_TRUE(parsed.wx.has_value());
  TEST_ASSERT_EQUAL_INT32(3, *parsed.wx);
  TEST_ASSERT_TRUE(parsed.alert.has_value());
  TEST_ASSERT_EQUAL_UINT8(2, *parsed.alert);
  TEST_ASSERT_TRUE(parsed.bikes.has_value());
  TEST_ASSERT_EQUAL_INT32(5, *parsed.bikes);
  TEST_ASSERT_TRUE(parsed.ebikes.has_value());
  TEST_ASSERT_EQUAL_INT32(2, *parsed.ebikes);
  TEST_ASSERT_TRUE(parsed.docks.has_value());
  TEST_ASSERT_EQUAL_INT32(7, *parsed.docks);
}

// A `bike` row (EventType::Bike, log schema v2) round-trips: stop_key "indego-<id>", route/dir/
// trip/vehicle empty, note = display name, bikes/ebikes/docks set.
static void test_csv_round_trip_bike_event(void) {
  LogEvent ev;
  ev.ts = 1757900000;
  ev.event = EventType::Bike;
  ev.stop_key = "indego-3468";
  ev.note = "Snyder & Dorrance";
  ev.bikes = 4;
  ev.ebikes = 2;
  ev.docks = 9;

  const std::string csv = transit_stats::toCsv(ev);
  TEST_ASSERT_TRUE(csv.find(",bike,") != std::string::npos);

  LogEvent parsed;
  TEST_ASSERT_TRUE(transit_stats::fromCsv(csv.c_str(), csv.size(), parsed));
  TEST_ASSERT_TRUE(parsed.event == EventType::Bike);
  TEST_ASSERT_EQUAL_STRING("indego-3468", parsed.stop_key.c_str());
  TEST_ASSERT_EQUAL_STRING("", parsed.route.c_str());
  TEST_ASSERT_EQUAL_STRING("Snyder & Dorrance", parsed.note.c_str());
  TEST_ASSERT_TRUE(parsed.bikes.has_value());
  TEST_ASSERT_EQUAL_INT32(4, *parsed.bikes);
  TEST_ASSERT_TRUE(parsed.ebikes.has_value());
  TEST_ASSERT_EQUAL_INT32(2, *parsed.ebikes);
  TEST_ASSERT_TRUE(parsed.docks.has_value());
  TEST_ASSERT_EQUAL_INT32(9, *parsed.docks);
}

// A pre-2026-09-14 14-column row (log schema v1, no trailing "seats,temp,wx,alert,bikes,ebikes,
// docks") still parses: the new fields all come back empty/unset, not an error.
static void test_csv_round_trip_v1_14_columns_still_parses(void) {
  const char* v1_line = "1757800000,arrive,17-21332,17,0,3667,7477,,1757800010,1757800012,3,,900,";
  LogEvent ev;
  TEST_ASSERT_TRUE(transit_stats::fromCsv(v1_line, std::string(v1_line).size(), ev));
  TEST_ASSERT_TRUE(ev.event == EventType::Arrive);
  TEST_ASSERT_EQUAL_STRING("17-21332", ev.stop_key.c_str());
  TEST_ASSERT_TRUE(ev.late_min.has_value());
  TEST_ASSERT_EQUAL_INT32(3, *ev.late_min);
  TEST_ASSERT_EQUAL_STRING("", ev.seats.c_str());
  TEST_ASSERT_FALSE(ev.temp.has_value());
  TEST_ASSERT_FALSE(ev.wx.has_value());
  TEST_ASSERT_FALSE(ev.alert.has_value());
  TEST_ASSERT_FALSE(ev.bikes.has_value());
  TEST_ASSERT_FALSE(ev.ebikes.has_value());
  TEST_ASSERT_FALSE(ev.docks.has_value());
}

// A line with a column count that is neither 14 (v1) nor 21 (v2) is rejected, not silently
// truncated/padded.
static void test_csv_wrong_column_count_is_rejected(void) {
  const char* bad = "1,arrive,S,17,0,T1,,,,,,,,,,,,,,";  // 20 fields, one short of v2
  LogEvent ev;
  TEST_ASSERT_FALSE(transit_stats::fromCsv(bad, std::string(bad).size(), ev));
}

// =============================================================================================
// events.h/.cpp: seatsToken / seatsLevel (log schema v2 crowding column).
// =============================================================================================

static void test_seats_token_mapping(void) {
  TEST_ASSERT_EQUAL_STRING("empty", transit_stats::seatsToken("EMPTY"));
  TEST_ASSERT_EQUAL_STRING("open", transit_stats::seatsToken("MANY_SEATS_AVAILABLE"));
  TEST_ASSERT_EQUAL_STRING("few", transit_stats::seatsToken("FEW_SEATS_AVAILABLE"));
  TEST_ASSERT_EQUAL_STRING("standing", transit_stats::seatsToken("STANDING_ROOM_ONLY"));
  TEST_ASSERT_EQUAL_STRING("packed", transit_stats::seatsToken("CRUSHED_STANDING_ROOM_ONLY"));
  TEST_ASSERT_EQUAL_STRING("full", transit_stats::seatsToken("FULL"));
  TEST_ASSERT_EQUAL_STRING("", transit_stats::seatsToken(""));
  TEST_ASSERT_EQUAL_STRING("", transit_stats::seatsToken("SOMETHING_UNKNOWN"));
}

static void test_seats_level_mapping(void) {
  TEST_ASSERT_EQUAL_INT(0, transit_stats::seatsLevel("empty"));
  TEST_ASSERT_EQUAL_INT(1, transit_stats::seatsLevel("open"));
  TEST_ASSERT_EQUAL_INT(2, transit_stats::seatsLevel("few"));
  TEST_ASSERT_EQUAL_INT(3, transit_stats::seatsLevel("standing"));
  TEST_ASSERT_EQUAL_INT(4, transit_stats::seatsLevel("packed"));
  TEST_ASSERT_EQUAL_INT(5, transit_stats::seatsLevel("full"));
  TEST_ASSERT_EQUAL_INT(-1, transit_stats::seatsLevel(""));
  TEST_ASSERT_EQUAL_INT(-1, transit_stats::seatsLevel("nonsense"));
}

// =============================================================================================
// aggregate.h/.cpp: America/New_York local time, verified against ground truth from the host's
// IANA tzdata (`TZ=America/New_York date -d ...`), independent of this project's own DST math.
// =============================================================================================

static void test_america_new_york_local_time(void) {
  int hour = -1, wd = -1;

  // 2026-01-15 08:00:00 America/New_York (EST) == 1768482000 UTC, a Thursday.
  transit_stats::americaNewYorkLocalHourWeekday(1768482000, hour, wd);
  TEST_ASSERT_EQUAL_INT(8, hour);
  TEST_ASSERT_EQUAL_INT(4, wd);

  // 2026-07-15 17:00:00 America/New_York (EDT) == 1784149200 UTC, a Wednesday.
  transit_stats::americaNewYorkLocalHourWeekday(1784149200, hour, wd);
  TEST_ASSERT_EQUAL_INT(17, hour);
  TEST_ASSERT_EQUAL_INT(3, wd);

  // Spring forward 2026: 2026-03-08 (2nd Sunday of March) 01:59:59 EST -> 03:00:00 EDT.
  transit_stats::americaNewYorkLocalHourWeekday(1772953199, hour, wd);
  TEST_ASSERT_EQUAL_INT(1, hour);
  TEST_ASSERT_EQUAL_INT(0, wd);  // Sunday
  transit_stats::americaNewYorkLocalHourWeekday(1772953200, hour, wd);
  TEST_ASSERT_EQUAL_INT(3, hour);
  TEST_ASSERT_EQUAL_INT(0, wd);

  // Fall back 2026: 2026-11-01 (1st Sunday of November) 01:59:59 EDT -> 01:00:00 EST.
  transit_stats::americaNewYorkLocalHourWeekday(1793512799, hour, wd);
  TEST_ASSERT_EQUAL_INT(1, hour);
  TEST_ASSERT_EQUAL_INT(0, wd);
  transit_stats::americaNewYorkLocalHourWeekday(1793512800, hour, wd);
  TEST_ASSERT_EQUAL_INT(1, hour);
  TEST_ASSERT_EQUAL_INT(0, wd);
}

// =============================================================================================
// tracker.h/.cpp: ArrivalTracker scenarios
// =============================================================================================

// (a) A bus first seen at ETA 20 min, polled every 30s as it approaches, then passes: exactly
// the expected `pred` rows at each horizon milestone, and one `arrive` with actual_ts equal to
// the last prediction and the correct headway relative to a previous bus at this stop.
static void test_tracker_pred_horizons_and_arrive_with_headway(void) {
  ArrivalTracker tracker;
  tracker.registerStop("S1", "17", "0");
  std::vector<LogEvent> out;

  // 2033-05-17 19:33:20 America/New_York. Deliberately mid-evening, not near midnight: this test
  // asserts a headway between bus A and bus B, and the tracker refuses to join a headway across a
  // local service-day boundary (tracker.h / DESIGN §9.2), which the old t0 of 2000000000
  // (23:33 local) would have straddled.
  const transit::Epoch t0 = 1999985600;

  // Bus A: sighted right as it arrives, then vanishes -> establishes the previous `arrive`.
  {
    transit::StopSnapshot snap;
    snap.key = "S1";
    transit::Arrival a;
    a.trip = "A";
    a.vehicle = "VA";
    a.predicted = t0;
    a.scheduled = t0 - 60;
    a.late_min = 1;
    a.late_known = true;
    a.status = transit::Status::Live;
    snap.arrivals.push_back(a);
    tracker.observe(snap, t0, true, true, out);
  }
  {
    // Two consecutive SUCCESSFUL observations without A are what it takes to conclude it passed
    // (kMissesBeforeInference, tracker.h): one missing observation is routinely a partial feed.
    transit::StopSnapshot snap;
    snap.key = "S1";  // A no longer present
    tracker.observe(snap, t0 + 30, true, true, out);
    tracker.observe(snap, t0 + 60, true, true, out);
  }

  size_t a_arrives = 0;
  for (const auto& ev : out) {
    if (ev.event == EventType::Arrive && ev.trip == "A") a_arrives++;
  }
  TEST_ASSERT_EQUAL_UINT32(1, a_arrives);

  // Bus B: first seen at ETA 20 min (1200s), polled every 30s, predicted time never changes
  // (it runs exactly on its own prediction) until it passes.
  const transit::Epoch t1 = t0 + 1000;
  const transit::Epoch predicted_B = t1 + 1200;
  for (transit::Epoch now = t1; now <= predicted_B; now += 30) {
    transit::StopSnapshot snap;
    snap.key = "S1";
    transit::Arrival a;
    a.trip = "B";
    a.vehicle = "VB";
    a.predicted = predicted_B;
    a.scheduled = predicted_B - 120;
    a.late_min = 2;
    a.late_known = true;
    a.status = transit::Status::Live;
    snap.arrivals.push_back(a);
    tracker.observe(snap, now, true, true, out);
  }
  {
    transit::StopSnapshot snap;
    snap.key = "S1";  // B has passed
    tracker.observe(snap, predicted_B + 30, true, true, out);
    tracker.observe(snap, predicted_B + 60, true, true, out);
  }

  std::vector<int32_t> b_pred_horizons;
  LogEvent b_arrive;
  bool found_b_arrive = false;
  for (const auto& ev : out) {
    if (ev.trip != "B") continue;
    if (ev.event == EventType::Pred) {
      TEST_ASSERT_TRUE(ev.horizon_s.has_value());
      b_pred_horizons.push_back(*ev.horizon_s);
    } else if (ev.event == EventType::Arrive) {
      b_arrive = ev;
      found_b_arrive = true;
    }
  }

  // First sighting (1200s) + crossing under 900/600/300/120 = 5 pred rows.
  const std::vector<int32_t> expected_horizons = {1200, 870, 570, 270, 90};
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(expected_horizons.size()),
                            static_cast<uint32_t>(b_pred_horizons.size()));
  for (size_t i = 0; i < expected_horizons.size() && i < b_pred_horizons.size(); i++) {
    TEST_ASSERT_EQUAL_INT32(expected_horizons[i], b_pred_horizons[i]);
  }

  TEST_ASSERT_TRUE(found_b_arrive);
  TEST_ASSERT_TRUE(b_arrive.actual_ts.has_value());
  TEST_ASSERT_EQUAL_INT64(predicted_B, *b_arrive.actual_ts);  // last prediction, within tolerance
  TEST_ASSERT_TRUE(b_arrive.headway_s.has_value());
  TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(predicted_B - t0), *b_arrive.headway_s);

  StopCounters ctr;
  TEST_ASSERT_TRUE(tracker.getStopCounters("S1", ctr));
  TEST_ASSERT_EQUAL_UINT32(2, ctr.arrivals_seen);  // A and B
}

// (b) A trip vanishes while its last prediction was still well over 180s in the future -> ghost,
// never an arrive.
static void test_tracker_ghost(void) {
  ArrivalTracker tracker;
  tracker.registerStop("S1", "17", "0");
  std::vector<LogEvent> out;

  const transit::Epoch t0 = 3000000000;
  {
    transit::StopSnapshot snap;
    snap.key = "S1";
    transit::Arrival a;
    a.trip = "G";
    a.vehicle = "VG";
    a.predicted = t0 + 400;
    a.scheduled = t0 + 400;
    a.late_known = false;
    a.status = transit::Status::Live;
    snap.arrivals.push_back(a);
    tracker.observe(snap, t0, true, true, out);
  }
  {
    transit::StopSnapshot snap;
    snap.key = "S1";  // G vanished while still 370s out (> 180s threshold)
    tracker.observe(snap, t0 + 30, true, true, out);
    tracker.observe(snap, t0 + 60, true, true, out);  // second successful miss confirms it
  }

  size_t ghosts = 0, arrives = 0;
  for (const auto& ev : out) {
    if (ev.trip != "G") continue;
    if (ev.event == EventType::Ghost) ghosts++;
    if (ev.event == EventType::Arrive) arrives++;
  }
  TEST_ASSERT_EQUAL_UINT32(1, ghosts);
  TEST_ASSERT_EQUAL_UINT32(0, arrives);

  StopCounters ctr;
  TEST_ASSERT_TRUE(tracker.getStopCounters("S1", ctr));
  TEST_ASSERT_EQUAL_UINT32(1, ctr.ghosts_seen);
}

// (c) noshow vs outage: an identical late scheduled-only arrival is a `noshow` when the route
// had live vehicles throughout the wait, and folds into `outage` (note="no_live_vehicles") when
// it did not.
static void test_tracker_noshow_vs_outage(void) {
  ArrivalTracker tracker;
  tracker.registerStop("S2a", "17", "0");
  tracker.registerStop("S2b", "17", "1");
  std::vector<LogEvent> out;

  const transit::Epoch t0 = 4000000000;

  // S2a: route_has_live_vehicles = true at every observe() call.
  {
    transit::StopSnapshot snap;
    snap.key = "S2a";
    transit::Arrival a;
    a.trip = "N1";
    a.scheduled = t0;
    a.status = transit::Status::Scheduled;
    snap.arrivals.push_back(a);
    tracker.observe(snap, t0, /*route_has_live_vehicles=*/true, true, out);
  }
  {
    transit::StopSnapshot snap;
    snap.key = "S2a";
    tracker.observe(snap, t0 + 601, /*route_has_live_vehicles=*/true, true, out);
  }

  // S2b: route_has_live_vehicles = false at every observe() call.
  {
    transit::StopSnapshot snap;
    snap.key = "S2b";
    transit::Arrival a;
    a.trip = "N2";
    a.scheduled = t0;
    a.status = transit::Status::Scheduled;
    snap.arrivals.push_back(a);
    tracker.observe(snap, t0, /*route_has_live_vehicles=*/false, true, out);
  }
  {
    transit::StopSnapshot snap;
    snap.key = "S2b";
    tracker.observe(snap, t0 + 601, /*route_has_live_vehicles=*/false, true, out);
  }

  bool found_noshow_n1 = false, found_outage_n2 = false;
  for (const auto& ev : out) {
    if (ev.event == EventType::NoShow && ev.trip == "N1" && ev.stop_key == "S2a") found_noshow_n1 = true;
    if (ev.event == EventType::Outage && ev.trip == "N2" && ev.stop_key == "S2b" &&
        ev.note == "no_live_vehicles") {
      found_outage_n2 = true;
    }
    // Neither should have been misclassified as the other event type.
    TEST_ASSERT_FALSE(ev.event == EventType::NoShow && ev.trip == "N2");
    TEST_ASSERT_FALSE(ev.event == EventType::Outage && ev.trip == "N1");
  }
  TEST_ASSERT_TRUE(found_noshow_n1);
  TEST_ASSERT_TRUE(found_outage_n2);
}

// (d) outage start/end: one row when poll_ok has been false for > 300s, one more (note="end")
// when polling recovers; no duplicate start while still failing. The start row is timestamped
// with the FIRST failed poll, not with the moment the 5-minute threshold was crossed (F23) --
// otherwise every outage in the log is short by the detection delay.
static void test_tracker_outage_start_end(void) {
  ArrivalTracker tracker;
  tracker.registerStop("S3", "17", "0");
  std::vector<LogEvent> out;
  transit::StopSnapshot empty;
  empty.key = "S3";

  const transit::Epoch t0 = 5000000000;
  tracker.observe(empty, t0, true, /*poll_ok=*/false, out);        // poll starts failing
  tracker.observe(empty, t0 + 100, true, /*poll_ok=*/false, out);  // still < 300s, no event yet
  tracker.observe(empty, t0 + 310, true, /*poll_ok=*/false, out);  // > 300s -> outage start
  tracker.observe(empty, t0 + 340, true, /*poll_ok=*/false, out);  // already emitted, no duplicate
  tracker.observe(empty, t0 + 400, true, /*poll_ok=*/true, out);   // recovers -> outage end

  std::vector<LogEvent> outages;
  for (const auto& ev : out) {
    if (ev.event == EventType::Outage) outages.push_back(ev);
  }
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(outages.size()));
  TEST_ASSERT_EQUAL_STRING("", outages[0].note.c_str());
  TEST_ASSERT_EQUAL_INT64(t0, outages[0].ts);  // first failure, NOT t0 + 310 (F23)
  TEST_ASSERT_TRUE(outages[0].horizon_s.has_value());
  TEST_ASSERT_EQUAL_INT32(310, *outages[0].horizon_s);  // detection delay is kept, not lost
  TEST_ASSERT_EQUAL_STRING("end", outages[1].note.c_str());
  TEST_ASSERT_EQUAL_INT64(t0 + 400, outages[1].ts);

  // The pair the aggregator will read therefore spans the whole 400 s, not 90 s.
  TEST_ASSERT_EQUAL_INT64(400, outages[1].ts - outages[0].ts);
}

// (e) Memory bound: more than kMaxTrackedTripsPerStop distinct trips at one stop, and more than
// kMaxTrackedStops distinct stops, both stay inside the fixed arrays and report what they did
// rather than growing without bound. The per-stop overflow is now a DROP, not an eviction: the
// 13th trip is the farthest out of the thirteen, and evicting a sooner trip to make room for it
// would be exactly the churn F19 is about.
static void test_tracker_memory_bound_eviction(void) {
  {
    ArrivalTracker tracker;
    tracker.registerStop("S4", "17", "0");
    std::vector<LogEvent> out;

    transit::StopSnapshot snap;
    snap.key = "S4";
    for (int i = 1; i <= (int)transit_stats::kMaxTrackedTripsPerStop + 1; i++) {  // one more than the per-stop cap
      transit::Arrival a;
      a.trip = "T" + std::to_string(i);
      a.predicted = 6000000000 + i;
      a.status = transit::Status::Live;
      snap.arrivals.push_back(a);
    }
    tracker.observe(snap, 6000000000, true, true, out);

    StopCounters ctr;
    TEST_ASSERT_TRUE(tracker.getStopCounters("S4", ctr));
    TEST_ASSERT_EQUAL_UINT32(0, ctr.evicted_trips);
    TEST_ASSERT_EQUAL_UINT32(1, ctr.dropped_observations);

    // The trips that kept their slots are the soonest ones, and the dropped one produced no row.
    size_t preds_for_last = 0;
    const std::string last_trip = "T" + std::to_string((int)transit_stats::kMaxTrackedTripsPerStop + 1);
    for (const auto& ev : out) {
      if (ev.event == EventType::Pred && ev.trip == last_trip) preds_for_last++;
    }
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(preds_for_last));
  }
  {
    ArrivalTracker tracker;
    std::vector<LogEvent> out;
    // Written against kMaxTrackedStops rather than a literal, so it holds at whatever the cap is -
    // it was 8 until 0.3.2-rc2 and is 4 now, and a test that hard-codes the number is a test that
    // stops testing the thing the moment the number moves.
    const int over = (int)transit_stats::kMaxTrackedStops + 1;
    for (int i = 1; i <= over; i++) {  // one more than kMaxTrackedStops
      transit::StopSnapshot snap;
      snap.key = "K" + std::to_string(i);
      tracker.observe(snap, 7000000000 + i, true, true, out);
    }
    TEST_ASSERT_EQUAL_UINT8((uint8_t)transit_stats::kMaxTrackedStops, tracker.trackedStopCount());
    TEST_ASSERT_TRUE(tracker.evictedStopCount() >= 1);

    StopCounters ctr;
    TEST_ASSERT_FALSE(tracker.getStopCounters("K1", ctr));  // least-recently-touched, evicted
    TEST_ASSERT_TRUE(tracker.getStopCounters("K" + std::to_string(over), ctr));  // most recent, retained
  }
}

// (f) Log schema v2: `pred` rows carry the crowding token current at that sighting, and the
// `arrive` row (emitted after the trip has vanished, so there is no live Arrival left to read)
// carries the *last seen* crowding token.
static void test_tracker_seats_on_pred_and_arrive(void) {
  ArrivalTracker tracker;
  tracker.registerStop("S5", "17", "0");
  std::vector<LogEvent> out;

  const transit::Epoch t0 = 8000000000;
  const transit::Epoch predicted = t0 + 1200;

  {  // First sighting: horizon 1200s, seats "FEW_SEATS_AVAILABLE" -> "few".
    transit::StopSnapshot snap;
    snap.key = "S5";
    transit::Arrival a;
    a.trip = "SEAT1";
    a.vehicle = "V1";
    a.predicted = predicted;
    a.scheduled = predicted - 60;
    a.status = transit::Status::Live;
    a.seats = "FEW_SEATS_AVAILABLE";
    snap.arrivals.push_back(a);
    tracker.observe(snap, t0, true, true, out);
  }
  {  // Horizon drops to 800s (crosses the 900s milestone) -> second pred row; seats now "standing".
    transit::StopSnapshot snap;
    snap.key = "S5";
    transit::Arrival a;
    a.trip = "SEAT1";
    a.vehicle = "V1";
    a.predicted = predicted;
    a.scheduled = predicted - 60;
    a.status = transit::Status::Live;
    a.seats = "STANDING_ROOM_ONLY";
    snap.arrivals.push_back(a);
    tracker.observe(snap, t0 + 400, true, true, out);
  }
  {  // Vanishes right at its predicted time -> arrive, carrying the last-seen seats level.
    transit::StopSnapshot snap;
    snap.key = "S5";
    tracker.observe(snap, predicted + 10, true, true, out);
    tracker.observe(snap, predicted + 40, true, true, out);  // confirming second miss
  }

  std::vector<LogEvent> preds;
  LogEvent arrive_ev;
  bool found_arrive = false;
  for (const auto& ev : out) {
    if (ev.trip != "SEAT1") continue;
    if (ev.event == EventType::Pred) preds.push_back(ev);
    if (ev.event == EventType::Arrive) { arrive_ev = ev; found_arrive = true; }
  }

  TEST_ASSERT_TRUE(preds.size() >= 2);
  TEST_ASSERT_EQUAL_STRING("few", preds[0].seats.c_str());
  TEST_ASSERT_EQUAL_STRING("standing", preds[1].seats.c_str());

  TEST_ASSERT_TRUE(found_arrive);
  TEST_ASSERT_EQUAL_STRING("standing", arrive_ev.seats.c_str());
}

// =============================================================================================
// aggregate.h/.cpp: a synthetic month of CSV, fed line-by-line, with known statistics.
// =============================================================================================

namespace {

// Builds CSV lines directly (bypassing ArrivalTracker, which is tested on its own above) while
// tracking, as a side effect, the plain top-level "known-late" statistics a human could compute
// by hand from the same inputs -- so the top-level on_time_pct/mean_late_min assertions below
// compare StatsAggregator's output against an independently-accumulated expectation rather than
// a hand-multiplied constant (much less error-prone for ~1400 generated rows).
struct SyntheticMonth {
  std::vector<std::string> lines;
  uint32_t expected_samples = 0;
  uint32_t expected_on_time = 0;
  uint32_t expected_late_known_count = 0;
  int64_t expected_sum_late_known = 0;

  void plainArrive(transit::Epoch ts, const char* stop_key, const std::string& trip,
                    int32_t late_min) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = stop_key;
    ev.route = "17";
    ev.dir = "0";
    ev.trip = trip;
    ev.actual_ts = ts;
    ev.late_min = late_min;
    lines.push_back(transit_stats::toCsv(ev));

    expected_samples++;
    expected_late_known_count++;
    expected_sum_late_known += late_min;
    if (late_min >= 0 && late_min <= 5) expected_on_time++;  // SEPTA on-time, see aggregate.h
  }

  void arriveUnknownLate(transit::Epoch ts, const char* stop_key, const std::string& trip,
                          transit::Epoch actual_ts) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = stop_key;
    ev.route = "17";
    ev.dir = "0";
    ev.trip = trip;
    ev.actual_ts = actual_ts;
    lines.push_back(transit_stats::toCsv(ev));
    expected_samples++;
  }

  void headwayArrive(transit::Epoch ts, const char* stop_key, const std::string& trip,
                      transit::Epoch scheduled_ts, std::optional<int32_t> headway_s) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = stop_key;
    ev.route = "17";
    ev.dir = "0";
    ev.trip = trip;
    ev.actual_ts = ts;
    ev.scheduled_ts = scheduled_ts;
    ev.headway_s = headway_s;
    lines.push_back(transit_stats::toCsv(ev));
    expected_samples++;
  }

  void pred(transit::Epoch ts, const char* stop_key, const std::string& trip,
            transit::Epoch predicted_ts, int32_t horizon_s) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Pred;
    ev.stop_key = stop_key;
    ev.route = "17";
    ev.dir = "0";
    ev.trip = trip;
    ev.predicted_ts = predicted_ts;
    ev.horizon_s = horizon_s;
    lines.push_back(transit_stats::toCsv(ev));
  }

  void ghost(transit::Epoch ts, const char* stop_key, const std::string& trip) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Ghost;
    ev.stop_key = stop_key;
    ev.route = "17";
    ev.dir = "0";
    ev.trip = trip;
    lines.push_back(transit_stats::toCsv(ev));
  }

  void noshow(transit::Epoch ts, const char* stop_key, const std::string& trip) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::NoShow;
    ev.stop_key = stop_key;
    ev.route = "17";
    ev.dir = "0";
    ev.trip = trip;
    lines.push_back(transit_stats::toCsv(ev));
  }

  void outage(transit::Epoch ts, const char* stop_key, const std::string& note) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Outage;
    ev.stop_key = stop_key;
    ev.route = "17";
    ev.dir = "0";
    ev.note = note;
    lines.push_back(transit_stats::toCsv(ev));
  }
};

constexpr transit::Epoch kWindowStart = 1767243600;  // 2026-01-01 00:00:00 America/New_York (EST)
constexpr transit::Epoch kWindowEnd = 1769835600;    // 2026-01-31 00:00:00 America/New_York (+30d)
constexpr const char* kStop = "AGG1";

transit::Epoch localEpoch(int day, int hour) {
  // All of January 2026 is EST (no DST in range), so local wall-clock time advances in lock
  // step with UTC seconds here -- verified against `date` (see DESIGN discussion in the header
  // comment of test_america_new_york_local_time above).
  return kWindowStart + static_cast<int64_t>(day) * 86400 + static_cast<int64_t>(hour) * 3600;
}

SyntheticMonth buildSyntheticMonth() {
  SyntheticMonth m;

  // Hour 8: 30 days, late_min cycling -2/2/6 by day%3 (10 of each) -> mean 2.0, p50 2, p90 6.
  for (int d = 0; d < 30; d++) {
    int32_t late_min = (d % 3 == 0) ? -2 : (d % 3 == 1) ? 2 : 6;
    m.plainArrive(localEpoch(d, 8), kStop, "H8-" + std::to_string(d), late_min);
  }
  // Hour 17: 20 days, late_min constant 1 -> mean 1.0, p50 1, p90 1.
  for (int d = 0; d < 20; d++) {
    m.plainArrive(localEpoch(d, 17), kStop, "H17-" + std::to_string(d), 1);
  }
  // Background traffic at every other hour of the day, 2 samples/day for all 30 days, so the
  // dataset is on the order of DESIGN's suggested "about 1500 lines" for a synthetic month.
  for (int h = 0; h < 24; h++) {
    if (h == 8 || h == 17) continue;
    int32_t late_min = (h % 6) - 1;  // -1, 0, 1, 2, 3, 4 repeating
    for (int d = 0; d < 30; d++) {
      for (int rep = 0; rep < 2; rep++) {
        std::string trip = "F" + std::to_string(h) + "-" + std::to_string(d) + "-" + std::to_string(rep);
        m.plainArrive(localEpoch(d, h), kStop, trip, late_min);
      }
    }
  }

  // Headway: one seed arrival (no previous -> no headway sample), then 10 arrivals with a fixed
  // 600s scheduled gap and headway_s of 200 (x3, ratio 0.33 -> bunched), 600 (x4, ratio 1.0),
  // 1200 (x3, ratio 2.0 -> gapped).
  const transit::Epoch sgt0 = localEpoch(0, 10) + 10000;
  m.headwayArrive(sgt0, kStop, "HW-seed", sgt0, std::nullopt);
  const int32_t hpattern[10] = {200, 200, 200, 600, 600, 600, 600, 1200, 1200, 1200};
  for (int k = 1; k <= 10; k++) {
    m.headwayArrive(sgt0 + k, kStop, "HW-" + std::to_string(k), sgt0 + 600LL * k, hpattern[k - 1]);
  }

  // Prediction accuracy: pred/arrive pairs with known error = predicted_ts - actual_ts, per
  // horizon bucket. late_min is left unknown on these arrive rows so they don't perturb the
  // on_time/mean_late_min stats above.
  const transit::Epoch predBase = localEpoch(1, 9);
  int uid = 0;
  for (int i = 0; i < 8; i++) {  // bucket 120, error +30, n=8
    transit::Epoch predicted_ts = predBase + (uid++) * 100;
    std::string trip = "P120-" + std::to_string(i);
    m.pred(predicted_ts - 120, kStop, trip, predicted_ts, 120);
    m.arriveUnknownLate(predicted_ts, kStop, trip, predicted_ts - 30);
  }
  for (int i = 0; i < 6; i++) {  // bucket 300, error -20, n=6
    transit::Epoch predicted_ts = predBase + (uid++) * 100;
    std::string trip = "P300-" + std::to_string(i);
    m.pred(predicted_ts - 300, kStop, trip, predicted_ts, 300);
    m.arriveUnknownLate(predicted_ts, kStop, trip, predicted_ts + 20);
  }
  {
    const int32_t errors[4] = {50, 50, -10, -10};  // bucket 600, mixed errors, n=4
    for (int i = 0; i < 4; i++) {
      transit::Epoch predicted_ts = predBase + (uid++) * 100;
      std::string trip = "P600-" + std::to_string(i);
      m.pred(predicted_ts - 600, kStop, trip, predicted_ts, 600);
      m.arriveUnknownLate(predicted_ts, kStop, trip, predicted_ts - errors[i]);
    }
  }
  for (int i = 0; i < 2; i++) {  // bucket 900, error +15, n=2
    transit::Epoch predicted_ts = predBase + (uid++) * 100;
    std::string trip = "P900-" + std::to_string(i);
    m.pred(predicted_ts - 900, kStop, trip, predicted_ts, 900);
    m.arriveUnknownLate(predicted_ts, kStop, trip, predicted_ts - 15);
  }

  // A few ghosts and noshows.
  m.ghost(localEpoch(2, 9) + 0, kStop, "G0");
  m.ghost(localEpoch(2, 9) + 1, kStop, "G1");
  m.ghost(localEpoch(2, 9) + 2, kStop, "G2");
  m.noshow(localEpoch(2, 10) + 0, kStop, "N0");
  m.noshow(localEpoch(2, 10) + 1, kStop, "N1");

  // One 15-minute poll outage, plus one unrelated "folded into outage" row from the tracker's
  // noshow-vs-outage fallback (note="no_live_vehicles") which must NOT be paired into outage_min.
  const transit::Epoch outageStart = localEpoch(3, 0);
  m.outage(outageStart, kStop, "");
  m.outage(outageStart + 900, kStop, "end");
  m.outage(localEpoch(3, 5), kStop, "no_live_vehicles");

  // Noise the aggregator must ignore: a different stop, and a row outside the window. Built
  // directly (not via the accumulating helpers above) since these rows must NOT contribute to
  // expected_* -- the whole point is that the aggregator filters them out.
  {
    LogEvent ev;
    ev.ts = localEpoch(5, 8);
    ev.event = EventType::Arrive;
    ev.stop_key = "OTHERSTOP";
    ev.route = "17";
    ev.dir = "0";
    ev.trip = "X0";
    ev.actual_ts = ev.ts;
    ev.late_min = 3;
    m.lines.push_back(transit_stats::toCsv(ev));
  }
  {
    LogEvent ev;
    ev.ts = kWindowStart - 100;
    ev.event = EventType::Arrive;
    ev.stop_key = kStop;
    ev.route = "17";
    ev.dir = "0";
    ev.trip = "OOB";
    ev.actual_ts = ev.ts;
    ev.late_min = 2;
    m.lines.push_back(transit_stats::toCsv(ev));  // NOT tracked in expected_* -- must be ignored
  }

  return m;
}

}  // namespace

static void test_aggregator_synthetic_month(void) {
  SyntheticMonth m = buildSyntheticMonth();
  TEST_ASSERT_TRUE(m.lines.size() > 1000);  // "about 1500 lines" per DESIGN §9.3, same order of magnitude

  StatsAggregator agg(kStop, kWindowStart, kWindowEnd);

  // The header row must be ignored wherever it appears in the stream.
  const std::string header = transit_stats::csvHeader();
  agg.feedLine(header.c_str(), header.size());
  for (const auto& line : m.lines) agg.feedLine(line.c_str(), line.size());

  TEST_ASSERT_EQUAL_UINT32(m.expected_samples, agg.samples());
  TEST_ASSERT_EQUAL_UINT32(m.expected_late_known_count, agg.lateKnown());
  TEST_ASSERT_TRUE(agg.hasOnTime());

  // On-time % is over KNOWN-lateness samples only (F21). The month deliberately contains
  // arrivals with no late_min at all (the forecast-stability pairs below); counting those in the
  // denominator would quietly drag the figure down by ~1.4 points here, and to 50% on the
  // one-on-time-one-unknown case the dedicated test covers.
  const double expected_on_time_pct =
      m.expected_late_known_count ? 100.0 * m.expected_on_time / m.expected_late_known_count : 0.0;
  TEST_ASSERT_TRUE(m.expected_samples > m.expected_late_known_count);  // the distinction is exercised
  const double expected_mean_late =
      m.expected_late_known_count
          ? static_cast<double>(m.expected_sum_late_known) / m.expected_late_known_count
          : 0.0;
  TEST_ASSERT_FLOAT_WITHIN(0.05f, static_cast<float>(expected_on_time_pct),
                            static_cast<float>(agg.onTimePct()));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, static_cast<float>(expected_mean_late),
                            static_cast<float>(agg.meanLateMin()));

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);

  TEST_ASSERT_EQUAL_STRING(kStop, doc["stop"].as<const char*>());
  TEST_ASSERT_EQUAL_INT32(30, doc["days"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT32(m.expected_samples, doc["samples"].as<uint32_t>());

  ArduinoJson::JsonArray by_hour = doc["by_hour"];
  TEST_ASSERT_EQUAL_INT(24, static_cast<int>(by_hour.size()));
  TEST_ASSERT_EQUAL_INT(8, by_hour[8]["h"].as<int>());
  TEST_ASSERT_EQUAL_UINT16(30, by_hour[8]["n"].as<uint16_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 2.0f, by_hour[8]["mean"].as<float>());
  TEST_ASSERT_EQUAL_INT(2, by_hour[8]["p50"].as<int>());
  TEST_ASSERT_EQUAL_INT(6, by_hour[8]["p90"].as<int>());

  TEST_ASSERT_EQUAL_INT(17, by_hour[17]["h"].as<int>());
  TEST_ASSERT_EQUAL_UINT16(20, by_hour[17]["n"].as<uint16_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, by_hour[17]["mean"].as<float>());
  TEST_ASSERT_EQUAL_INT(1, by_hour[17]["p50"].as<int>());
  TEST_ASSERT_EQUAL_INT(1, by_hour[17]["p90"].as<int>());

  ArduinoJson::JsonArray by_weekday = doc["by_weekday"];
  TEST_ASSERT_EQUAL_INT(7, static_cast<int>(by_weekday.size()));

  ArduinoJson::JsonObject headway = doc["headway"];
  TEST_ASSERT_EQUAL_UINT16(10, headway["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT16(3, headway["bunched"].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT16(3, headway["gapped"].as<uint16_t>());
  ArduinoJson::JsonArray ratio_hist = headway["ratio_hist"];
  TEST_ASSERT_EQUAL_INT(20, static_cast<int>(ratio_hist.size()));
  TEST_ASSERT_EQUAL_UINT16(3, ratio_hist[3].as<uint16_t>());   // ratio 0.33
  TEST_ASSERT_EQUAL_UINT16(4, ratio_hist[10].as<uint16_t>());  // ratio 1.0
  TEST_ASSERT_EQUAL_UINT16(3, ratio_hist[19].as<uint16_t>());  // ratio 2.0, clamped

  TEST_ASSERT_EQUAL_UINT32(3, doc["ghost"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(2, doc["noshow"].as<uint32_t>());
  TEST_ASSERT_EQUAL_INT32(15, doc["outage_min"].as<int32_t>());  // only the paired start/end

  // F22: the feature is "forecast stability" (how much the forecast MOVED between a horizon and
  // the final inferred time), not "prediction accuracy". Same arithmetic, honest name and keys.
  TEST_ASSERT_TRUE(doc["prediction"].isNull());  // the misleading old key is gone, not aliased
  ArduinoJson::JsonArray stability = doc["forecast_stability"];
  TEST_ASSERT_EQUAL_INT(4, static_cast<int>(stability.size()));

  TEST_ASSERT_EQUAL_INT32(120, stability[0]["horizon_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(8, stability[0]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(30, stability[0]["mean_abs_revision_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(30, stability[0]["mean_revision_s"].as<int32_t>());

  TEST_ASSERT_EQUAL_INT32(300, stability[1]["horizon_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(6, stability[1]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(20, stability[1]["mean_abs_revision_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(-20, stability[1]["mean_revision_s"].as<int32_t>());

  TEST_ASSERT_EQUAL_INT32(600, stability[2]["horizon_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(4, stability[2]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(30, stability[2]["mean_abs_revision_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(20, stability[2]["mean_revision_s"].as<int32_t>());

  TEST_ASSERT_EQUAL_INT32(900, stability[3]["horizon_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(2, stability[3]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(15, stability[3]["mean_abs_revision_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(15, stability[3]["mean_revision_s"].as<int32_t>());

  // Every response says how much of the window it could actually see, and how many of its
  // arrivals are marked as inferred, so the UI never has to guess (F22/F23).
  TEST_ASSERT_EQUAL_UINT32(m.expected_late_known_count, doc["late_known"].as<uint32_t>());
  TEST_ASSERT_EQUAL_STRING("half_mean_gap", doc["wait_basis"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT32(0, doc["inferred"].as<uint32_t>());  // synthetic rows carry no marker
  // 15 minutes of outage in a 30-day window: coverage is ~0.9997, and certainly not 0 or 1.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9997f, doc["coverage"].as<float>());
}

static void test_summary_from_aggregator(void) {
  StatsAggregator agg("SUM1", 0, 1000000);
  LogEvent ev;
  ev.ts = 100;
  ev.event = EventType::Arrive;
  ev.stop_key = "SUM1";
  ev.actual_ts = 100;
  ev.late_min = 2;
  const std::string line = transit_stats::toCsv(ev);
  agg.feedLine(line.c_str(), line.size());

  StopSummary s = transit_stats::summarize(agg);
  TEST_ASSERT_EQUAL_UINT32(1, s.samples);
  TEST_ASSERT_EQUAL_UINT32(1, s.late_known);
  TEST_ASSERT_TRUE(s.has_on_time);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, s.on_time_pct);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, s.mean_late_min);
  TEST_ASSERT_EQUAL_UINT32(0, s.ghosts);

  // A window with no known-lateness sample at all must say so, not report 0% on time (F21).
  StatsAggregator empty("SUM2", 0, 1000000);
  StopSummary e = transit_stats::summarize(empty);
  TEST_ASSERT_EQUAL_UINT32(0, e.samples);
  TEST_ASSERT_EQUAL_UINT32(0, e.late_known);
  TEST_ASSERT_FALSE(e.has_on_time);
}

// =============================================================================================
// aggregate.h/.cpp: crowding and wait_by_hour (log schema v2), on a small synthetic log with a
// trivial, deterministic (not America/New_York) hour/weekday function -- the point of this test
// is the bucketing arithmetic, not timezone math (already covered by test_america_new_york_local_time).
// =============================================================================================

static void trivialHourWeekday(transit::Epoch utc, int& hour, int& weekday) {
  hour = static_cast<int>((utc / 3600) % 24);
  weekday = static_cast<int>((utc / 86400) % 7);
}

static void test_aggregator_crowding_and_wait_by_hour(void) {
  const char* kStop2 = "CWSTOP";
  StatsAggregator agg(kStop2, 0, 100000, &trivialHourWeekday);

  auto feed = [&](const LogEvent& ev) {
    const std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
  };
  auto arrive = [&](transit::Epoch ts, const std::string& trip, const char* seats,
                     std::optional<int32_t> headway_s) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = kStop2;
    ev.trip = trip;
    ev.actual_ts = ts;
    ev.seats = seats;
    ev.headway_s = headway_s;
    feed(ev);
  };

  // Hour 8 (ts 28800/28801/28802): two known-seats arrivals (few=2, standing=3) and one
  // unknown-seats arrival; two positive headways (300, 500) and one zero headway that must NOT
  // count toward wait_by_hour's n/mean/max (DESIGN: "known headway_s (>0)").
  arrive(28800, "C1", "few", 300);
  arrive(28801, "C2", "standing", 500);
  arrive(28802, "C3", "", 0);

  // Hour 9 (ts 32400): one known-seats arrival (full=5), no headway.
  arrive(32400, "C4", "full", std::nullopt);

  LogEvent g;
  g.ts = 28803;  // hour 8
  g.event = EventType::Ghost;
  g.stop_key = kStop2;
  g.trip = "G1";
  feed(g);

  LogEvent n;
  n.ts = 32401;  // hour 9
  n.event = EventType::NoShow;
  n.stop_key = kStop2;
  n.trip = "N1";
  feed(n);

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);

  ArduinoJson::JsonArray crowd_hour = doc["crowding"]["by_hour"];
  TEST_ASSERT_EQUAL_INT(24, static_cast<int>(crowd_hour.size()));
  TEST_ASSERT_EQUAL_UINT16(2, crowd_hour[8]["n"].as<uint16_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 2.5f, crowd_hour[8]["mean"].as<float>());
  ArduinoJson::JsonArray dist8 = crowd_hour[8]["dist"];
  TEST_ASSERT_EQUAL_INT(6, static_cast<int>(dist8.size()));
  TEST_ASSERT_EQUAL_UINT16(1, dist8[2].as<uint16_t>());  // few
  TEST_ASSERT_EQUAL_UINT16(1, dist8[3].as<uint16_t>());  // standing
  TEST_ASSERT_EQUAL_UINT16(1, crowd_hour[9]["n"].as<uint16_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.0f, crowd_hour[9]["mean"].as<float>());
  TEST_ASSERT_EQUAL_UINT16(0, crowd_hour[10]["n"].as<uint16_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, crowd_hour[10]["mean"].as<float>());  // always present, even n==0

  ArduinoJson::JsonArray crowd_wd = doc["crowding"]["by_weekday"];
  TEST_ASSERT_EQUAL_INT(7, static_cast<int>(crowd_wd.size()));
  // Every ts above is < 86400, so trivialHourWeekday puts them all on weekday 0.
  TEST_ASSERT_EQUAL_UINT16(3, crowd_wd[0]["n"].as<uint16_t>());  // C1, C2, C4 (C3's seats unknown)

  ArduinoJson::JsonArray wait_hour = doc["wait_by_hour"];
  TEST_ASSERT_EQUAL_INT(24, static_cast<int>(wait_hour.size()));
  TEST_ASSERT_EQUAL_UINT16(2, wait_hour[8]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(400, wait_hour[8]["mean_gap_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(500, wait_hour[8]["max_gap_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(1, wait_hour[8]["ghost"].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT16(0, wait_hour[8]["noshow"].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT16(0, wait_hour[9]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT16(1, wait_hour[9]["noshow"].as<uint16_t>());
}

// =============================================================================================
// overview.h/.cpp: OverviewAggregator on a synthetic log with two stops and one Indego station.
// =============================================================================================

static void test_overview_aggregator_stops_and_bikes(void) {
  OverviewAggregator agg(0, 200000, &trivialHourWeekday);

  auto feed = [&](const LogEvent& ev) {
    const std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
  };
  auto arrive = [&](const char* stop, transit::Epoch ts, const std::string& trip, int32_t late_min) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = stop;
    ev.trip = trip;
    ev.actual_ts = ts;
    ev.late_min = late_min;
    feed(ev);
  };

  const char* kOA1 = "17-21332";
  const char* kOA2 = "17-21297";

  // OA1: 4 arrivals, late -1/2/6/3 -> 2 on-time (2, 3; SEPTA on-time is [0,5]), mean 2.5.
  arrive(kOA1, 1000, "A1", -1);
  arrive(kOA1, 2000, "A2", 2);
  arrive(kOA1, 3000, "A3", 6);
  arrive(kOA1, 4000, "A4", 3);

  LogEvent oa1_ghost;
  oa1_ghost.ts = 4500;
  oa1_ghost.event = EventType::Ghost;
  oa1_ghost.stop_key = kOA1;
  oa1_ghost.trip = "GA1";
  feed(oa1_ghost);

  LogEvent oa1_noshow;
  oa1_noshow.ts = 5000;  // OA1's last_seen_ts: max ts of ANY row for that stop
  oa1_noshow.event = EventType::NoShow;
  oa1_noshow.stop_key = kOA1;
  oa1_noshow.trip = "NA1";
  feed(oa1_noshow);

  // OA2: 2 arrivals, late 0/1 -> both on-time, mean 0.5.
  arrive(kOA2, 1500, "B1", 0);
  arrive(kOA2, 2500, "B2", 1);

  const char* kStation = "indego-3468";
  auto bike = [&](transit::Epoch ts, int32_t bikes, int32_t ebikes, int32_t docks) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Bike;
    ev.stop_key = kStation;
    ev.note = "Snyder & Dorrance";
    ev.bikes = bikes;
    ev.ebikes = ebikes;
    ev.docks = docks;
    feed(ev);
  };
  bike(28800, 4, 2, 8);  // hour 8
  bike(28900, 6, 0, 6);  // hour 8
  bike(32400, 3, 1, 9);  // hour 9

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);

  TEST_ASSERT_EQUAL_INT32(2, doc["days"].as<int32_t>());  // (200000 - 0) / 86400, truncated

  ArduinoJson::JsonArray stops = doc["stops"];
  TEST_ASSERT_EQUAL_INT(2, static_cast<int>(stops.size()));

  TEST_ASSERT_EQUAL_STRING(kOA1, stops[0]["stop"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT32(4, stops[0]["samples"].as<uint32_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 50.0f, stops[0]["on_time_pct"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 2.5f, stops[0]["mean_late_min"].as<float>());
  TEST_ASSERT_EQUAL_UINT32(1, stops[0]["ghost"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(1, stops[0]["noshow"].as<uint32_t>());
  TEST_ASSERT_EQUAL_INT64(5000, stops[0]["last_seen_ts"].as<int64_t>());

  TEST_ASSERT_EQUAL_STRING(kOA2, stops[1]["stop"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT32(2, stops[1]["samples"].as<uint32_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 100.0f, stops[1]["on_time_pct"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.5f, stops[1]["mean_late_min"].as<float>());
  TEST_ASSERT_EQUAL_UINT32(0, stops[1]["ghost"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(0, stops[1]["noshow"].as<uint32_t>());
  TEST_ASSERT_EQUAL_INT64(2500, stops[1]["last_seen_ts"].as<int64_t>());

  ArduinoJson::JsonArray bikes = doc["bikes"];
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(bikes.size()));
  TEST_ASSERT_EQUAL_STRING(kStation, bikes[0]["station"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("Snyder & Dorrance", bikes[0]["name"].as<const char*>());

  ArduinoJson::JsonArray by_hour = bikes[0]["by_hour"];
  TEST_ASSERT_EQUAL_INT(24, static_cast<int>(by_hour.size()));
  TEST_ASSERT_EQUAL_UINT16(2, by_hour[8]["n"].as<uint16_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.0f, by_hour[8]["bikes"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, by_hour[8]["ebikes"].as<float>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 7.0f, by_hour[8]["docks"].as<float>());
  TEST_ASSERT_EQUAL_UINT16(1, by_hour[9]["n"].as<uint16_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 3.0f, by_hour[9]["bikes"].as<float>());
  TEST_ASSERT_EQUAL_UINT16(0, by_hour[10]["n"].as<uint16_t>());  // always present, even n==0
}

// Fixed capacity (DESIGN §9.3): rows for a 9th distinct stop_key or 4th distinct Indego station
// are ignored, not evicted -- the first kMaxOverviewStops/kMaxOverviewBikeStations keys seen win.
static void test_overview_aggregator_capacity_ignores_overflow(void) {
  OverviewAggregator agg(0, 200000, &trivialHourWeekday);
  auto feed = [&](const LogEvent& ev) {
    const std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
  };

  for (int i = 1; i <= 9; i++) {  // one more than kMaxOverviewStops (8)
    LogEvent ev;
    ev.ts = 1000 + i;
    ev.event = EventType::Arrive;
    ev.stop_key = "STOP" + std::to_string(i);
    ev.trip = "T" + std::to_string(i);
    ev.actual_ts = ev.ts;
    feed(ev);
  }
  for (int i = 1; i <= 4; i++) {  // one more than kMaxOverviewBikeStations (3)
    LogEvent ev;
    ev.ts = 2000 + i;
    ev.event = EventType::Bike;
    ev.stop_key = "indego-" + std::to_string(i);
    ev.note = "Station " + std::to_string(i);
    ev.bikes = i;
    feed(ev);
  }

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);

  ArduinoJson::JsonArray stops = doc["stops"];
  TEST_ASSERT_EQUAL_INT(8, static_cast<int>(stops.size()));
  TEST_ASSERT_EQUAL_STRING("STOP1", stops[0]["stop"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("STOP8", stops[7]["stop"].as<const char*>());

  ArduinoJson::JsonArray bikes = doc["bikes"];
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(bikes.size()));
  TEST_ASSERT_EQUAL_STRING("indego-1", bikes[0]["station"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("indego-3", bikes[2]["station"].as<const char*>());

  // What did not fit is REPORTED, not silently dropped, so the page can say "1 stop not shown"
  // instead of implying the list is everything (F28).
  TEST_ASSERT_EQUAL_UINT32(1, doc["excluded_stops"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(1, doc["excluded_bikes"].as<uint32_t>());
}

// =============================================================================================
// Memory budget (DESIGN.md §9.3: "all computed in one streaming pass with fixed-size
// accumulators", < 9 KB for StatsAggregator, and OverviewAggregator's own "aim < 2 KB").
// =============================================================================================

static void test_stats_aggregator_size_budget(void) {
  printf("sizeof(transit_stats::StatsAggregator)    = %zu bytes\n", sizeof(StatsAggregator));
  printf("sizeof(transit_stats::ArrivalTracker)      = %zu bytes\n", sizeof(ArrivalTracker));
  printf("sizeof(transit_stats::OverviewAggregator)  = %zu bytes\n", sizeof(OverviewAggregator));
  TEST_ASSERT_TRUE(sizeof(StatsAggregator) < 9216);
  // DESIGN §9.3 aims for < 2 KB; asserted here with headroom rather than the exact target, since
  // std::string SSO thresholds differ between the 64-bit host and the 32-bit ESP32 target.
  TEST_ASSERT_TRUE(sizeof(OverviewAggregator) < 4096);
  // kMaxTrackedTripsPerStop went 8 -> 12 for F19 and back to 8 in 0.3.1 (tracker.h says why, and
  // what it costs). The budget is ~20 KB on the 32-bit target; the 64-bit host build is the larger
  // of the two (std::string is 32 B there, 24 B on ESP32), so asserting it here is the strict case.
  TEST_ASSERT_TRUE(sizeof(ArrivalTracker) < 20480);
}

// =============================================================================================
// log_window.h/.cpp: which monthly CSV files a GET /api/stats?days=N window could touch.
// =============================================================================================

static transit::Epoch localEastern(int y, int mo, int d, int h, int mi, int s) {
  return transit::localToEpoch(y, mo, d, h, mi, s, transit::kUsEastern);
}

static void test_months_in_window_single_day_one_month(void) {
  // A one-day window entirely inside September 2026 should name just that month.
  transit::Epoch start = localEastern(2026, 9, 13, 0, 0, 0);
  transit::Epoch end = localEastern(2026, 9, 14, 0, 0, 0);
  std::vector<std::string> months = transit_stats::monthsInWindow(start, end);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(months.size()));
  TEST_ASSERT_EQUAL_STRING("2026-09", months[0].c_str());
}

static void test_months_in_window_spans_month_boundary(void) {
  // days=30 ending partway through September should reach back into August.
  transit::Epoch end = localEastern(2026, 9, 13, 12, 0, 0);
  transit::Epoch start = end - 30 * 86400;
  std::vector<std::string> months = transit_stats::monthsInWindow(start, end);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(months.size()));
  TEST_ASSERT_EQUAL_STRING("2026-08", months[0].c_str());
  TEST_ASSERT_EQUAL_STRING("2026-09", months[1].c_str());
}

static void test_months_in_window_spans_year_boundary(void) {
  transit::Epoch start = localEastern(2025, 12, 20, 0, 0, 0);
  transit::Epoch end = localEastern(2026, 1, 10, 0, 0, 0);
  std::vector<std::string> months = transit_stats::monthsInWindow(start, end);
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(months.size()));
  TEST_ASSERT_EQUAL_STRING("2025-12", months[0].c_str());
  TEST_ASSERT_EQUAL_STRING("2026-01", months[1].c_str());
}

static void test_months_in_window_many_months() {
  // days=90 from DESIGN.md's example (§9.3 shows "days": 30, but the API accepts any N).
  transit::Epoch end = localEastern(2026, 3, 1, 0, 0, 0);
  transit::Epoch start = end - 90 * 86400;
  std::vector<std::string> months = transit_stats::monthsInWindow(start, end);
  // Dec, Jan, Feb, (Mar's first instant is excluded - window_end is exclusive at exactly
  // midnight, so it never touches March at all).
  TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(months.size()));
  TEST_ASSERT_EQUAL_STRING("2025-12", months[0].c_str());
  TEST_ASSERT_EQUAL_STRING("2026-01", months[1].c_str());
  TEST_ASSERT_EQUAL_STRING("2026-02", months[2].c_str());
}

static void test_months_in_window_empty_when_end_not_after_start(void) {
  transit::Epoch t = localEastern(2026, 9, 13, 0, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(transit_stats::monthsInWindow(t, t).size()));
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(transit_stats::monthsInWindow(t, t - 10).size()));
}

static void test_months_in_window_across_dst_spring_forward(void) {
  // 2026's US "spring forward" is 2026-03-08 02:00 local. A window straddling it in UTC must
  // not miscompute the local month on either side (this exercises the DST branch in
  // localYearMonth(), not just the always-EST winter months every other test above uses).
  transit::Epoch start = localEastern(2026, 3, 1, 0, 0, 0);
  transit::Epoch end = localEastern(2026, 3, 31, 23, 59, 59);
  std::vector<std::string> months = transit_stats::monthsInWindow(start, end);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(months.size()));
  TEST_ASSERT_EQUAL_STRING("2026-03", months[0].c_str());
}

// =============================================================================================
// Regression tests for the 2026-09-15 measurement-defect review (F17-F25, F28). Each one asserts
// the CORRECT behaviour for a defect the review reproduced; the comment on each says what the
// device used to report and why that was wrong.
// =============================================================================================

namespace {

// 2026-01-01 12:00:00 America/New_York. Midday on purpose: several of these tests assert a
// headway, and headway continuity deliberately stops at a local service-day boundary.
constexpr transit::Epoch kT = 1767286800;

transit::Arrival liveArrival(const std::string& trip, transit::Epoch predicted,
                              transit::Epoch scheduled = 0) {
  transit::Arrival a;
  a.trip = trip;
  a.vehicle = "V" + trip;
  a.predicted = predicted;
  a.scheduled = scheduled;
  a.status = transit::Status::Live;
  return a;
}

transit::Arrival schedArrival(const std::string& trip, transit::Epoch scheduled) {
  transit::Arrival a;
  a.trip = trip;
  a.scheduled = scheduled;
  a.status = transit::Status::Scheduled;
  return a;
}

transit::StopSnapshot snapOf(const char* key, const std::vector<transit::Arrival>& arrivals) {
  transit::StopSnapshot s;
  s.key = key;
  s.arrivals = arrivals;
  return s;
}

size_t countOf(const std::vector<LogEvent>& out, EventType type) {
  size_t n = 0;
  for (const auto& ev : out) {
    if (ev.event == type) n++;
  }
  return n;
}

size_t countOfTrip(const std::vector<LogEvent>& out, EventType type, const std::string& trip) {
  size_t n = 0;
  for (const auto& ev : out) {
    if (ev.event == type && ev.trip == trip) n++;
  }
  return n;
}

// No event anywhere may carry a negative (or zero-as-if-real) headway: the aggregator would count
// it as a gap and could classify it as bunching (F20).
void assertNoNegativeHeadways(const std::vector<LogEvent>& out) {
  for (const auto& ev : out) {
    if (ev.headway_s.has_value()) TEST_ASSERT_TRUE(*ev.headway_s > 0);
  }
}

}  // namespace

// ---- F17: a failed poll is not evidence --------------------------------------------------

// The review's reproduction: a live trip predicted for T+60, then an EMPTY snapshot at T+30 with
// poll_ok=false. The tracker used to reap the trip anyway and emit an `arrive` -- a network
// timeout inventing a bus arrival. Nothing here may produce arrive/ghost/noshow/headway.
static void test_tracker_failed_poll_creates_no_arrival(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F17", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F17", {liveArrival("L", kT + 60), schedArrival("S", kT + 60)}), kT, true,
                   true, out);
  out.clear();  // the first-sighting pred row is not what this test is about

  // Eleven minutes of failed polls: long enough to cross the 5-minute outage threshold AND to
  // pass the live trip's prediction and the scheduled trip's 10-minute no-show deadline.
  for (transit::Epoch t = kT + 30; t <= kT + 690; t += 30) {
    tracker.observe(snapOf("F17", {}), t, true, /*poll_ok=*/false, out);
  }

  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(out, EventType::Arrive)));
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(out, EventType::Ghost)));
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(out, EventType::NoShow)));
  assertNoNegativeHeadways(out);
  for (const auto& ev : out) TEST_ASSERT_FALSE(ev.headway_s.has_value());

  // The one thing a failed poll IS evidence of: an outage, dated from the first failure (F23).
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(countOf(out, EventType::Outage)));
  TEST_ASSERT_EQUAL_INT64(kT + 30, out[0].ts);
}

// On recovery, a trip whose predicted time passed while we were blind is closed exactly once, as
// an explicitly "unobserved" passage -- not fabricated as an ordinary arrival, and not left to
// rot until it looks like a ghost.
static void test_tracker_outage_recovery_closes_unobserved_once(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F17b", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F17b", {liveArrival("U", kT + 300)}), kT, true, true, out);
  for (transit::Epoch t = kT + 30; t <= kT + 630; t += 30) {
    tracker.observe(snapOf("F17b", {}), t, true, /*poll_ok=*/false, out);
  }
  out.clear();

  tracker.observe(snapOf("F17b", {}), kT + 660, true, /*poll_ok=*/true, out);  // recovery
  const size_t arrives_after_recovery = countOf(out, EventType::Arrive);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(arrives_after_recovery));

  LogEvent closed;
  for (const auto& ev : out) {
    if (ev.event == EventType::Arrive) closed = ev;
  }
  TEST_ASSERT_EQUAL_STRING("unobserved", closed.note.c_str());
  TEST_ASSERT_TRUE(closed.actual_ts.has_value());
  TEST_ASSERT_EQUAL_INT64(kT + 300, *closed.actual_ts);  // its last prediction; the note says so
  TEST_ASSERT_FALSE(closed.headway_s.has_value());       // we do not know it was the next bus

  // And it is gone: further successful polls must not re-close it.
  tracker.observe(snapOf("F17b", {}), kT + 690, true, true, out);
  tracker.observe(snapOf("F17b", {}), kT + 720, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(countOf(out, EventType::Arrive)));

  StopCounters ctr;
  TEST_ASSERT_TRUE(tracker.getStopCounters("F17b", ctr));
  TEST_ASSERT_EQUAL_UINT32(1, ctr.unobserved_arrivals);
}

// The other half of "do not double count": if the trip is STILL being predicted when polling
// recovers, it was not an unobserved passage at all. It keeps being tracked and produces exactly
// one arrival later, when it genuinely disappears.
static void test_tracker_outage_recovery_no_double_count(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F17c", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F17c", {liveArrival("R", kT + 300)}), kT, true, true, out);
  for (transit::Epoch t = kT + 30; t <= kT + 630; t += 30) {
    tracker.observe(snapOf("F17c", {}), t, true, /*poll_ok=*/false, out);
  }
  // Recovery, and the bus is still there, running late.
  tracker.observe(snapOf("F17c", {liveArrival("R", kT + 700)}), kT + 660, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(out, EventType::Arrive)));

  tracker.observe(snapOf("F17c", {}), kT + 690, true, true, out);
  tracker.observe(snapOf("F17c", {}), kT + 720, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(countOfTrip(out, EventType::Arrive, "R")));

  for (const auto& ev : out) {
    if (ev.event == EventType::Arrive) {
      TEST_ASSERT_EQUAL_STRING("inferred", ev.note.c_str());  // not "unobserved"
      TEST_ASSERT_EQUAL_INT64(kT + 700, *ev.actual_ts);
    }
  }
}

// Genuine disappearance in fresh, successful data still follows the documented heuristic -- it
// just needs kMissesBeforeInference consecutive successful polls to agree.
static void test_tracker_inference_needs_two_successful_misses(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F17d", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F17d", {liveArrival("D", kT + 60)}), kT, true, true, out);
  tracker.observe(snapOf("F17d", {}), kT + 90, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(out, EventType::Arrive)));

  tracker.observe(snapOf("F17d", {}), kT + 120, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(countOf(out, EventType::Arrive)));

  // A trip that merely blinked out of one feed and came back is NOT an arrival.
  ArrivalTracker blink;
  blink.registerStop("F17e", "17", "0");
  std::vector<LogEvent> bout;
  blink.observe(snapOf("F17e", {liveArrival("B", kT + 600)}), kT, true, true, bout);
  blink.observe(snapOf("F17e", {}), kT + 30, true, true, bout);                       // partial feed
  blink.observe(snapOf("F17e", {liveArrival("B", kT + 600)}), kT + 60, true, true, bout);
  blink.observe(snapOf("F17e", {}), kT + 90, true, true, bout);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(bout, EventType::Arrive)));
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(bout, EventType::Ghost)));
}

// ---- F18: a scheduled trip that turns up live is not also a no-show ----------------------

// The review's reproduction: schedule row for trip "same" at T+30, the live vehicle for the same
// trip at T+10, then disappearance. The tracker keyed its lookups by (trip id, kind), so the
// pending schedule entry was never retired and the one bus produced an `arrive` AND a `noshow`.
static void test_tracker_scheduled_then_live_same_id_no_noshow(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F18a", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F18a", {schedArrival("same", kT + 30)}), kT, true, true, out);
  tracker.observe(snapOf("F18a", {liveArrival("same", kT + 30, kT + 30)}), kT + 10, true, true, out);
  tracker.observe(snapOf("F18a", {}), kT + 40, true, true, out);
  tracker.observe(snapOf("F18a", {}), kT + 70, true, true, out);
  tracker.observe(snapOf("F18a", {}), kT + 931, true, true, out);  // well past the no-show deadline

  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(countOf(out, EventType::Arrive)));
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(out, EventType::NoShow)));
}

// The realistic SEPTA shape: BusSchedules' static trip id and GTFS-RT's realtime trip id are
// different strings for the same bus (DESIGN §4.4), so the join has to fall back to the matching
// scheduled time. (transit_core is adding Arrival::sched_trip for a precise join; this must work
// without it.)
static void test_tracker_scheduled_then_live_different_ids_same_scheduled_time(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F18b", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F18b", {schedArrival("SCHED-88431", kT + 30)}), kT, true, true, out);
  tracker.observe(snapOf("F18b", {liveArrival("3667", kT + 30, kT + 30)}), kT + 10, true, true, out);
  tracker.observe(snapOf("F18b", {}), kT + 40, true, true, out);
  tracker.observe(snapOf("F18b", {}), kT + 70, true, true, out);
  tracker.observe(snapOf("F18b", {}), kT + 931, true, true, out);

  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(countOfTrip(out, EventType::Arrive, "3667")));
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(out, EventType::NoShow)));

  // The reverse order (live vehicle first, schedule row after) must not open a pending record
  // either -- it is the same bus seen twice.
  ArrivalTracker rev;
  rev.registerStop("F18c", "17", "0");
  std::vector<LogEvent> rout;
  rev.observe(snapOf("F18c", {liveArrival("3668", kT + 30, kT + 30)}), kT, true, true, rout);
  rev.observe(snapOf("F18c", {liveArrival("3668", kT + 30, kT + 30), schedArrival("SCHED-2", kT + 30)}),
               kT + 10, true, true, rout);
  rev.observe(snapOf("F18c", {}), kT + 40, true, true, rout);
  rev.observe(snapOf("F18c", {}), kT + 70, true, true, rout);
  rev.observe(snapOf("F18c", {}), kT + 931, true, true, rout);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(rout, EventType::NoShow)));
}

// A scheduled trip that never shows up live, with the route running normally throughout, is still
// a no-show. Fixing F18 must not silence the real signal.
static void test_tracker_never_observed_scheduled_still_noshow(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F18d", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F18d", {schedArrival("ghosted", kT + 30)}), kT, true, true, out);
  tracker.observe(snapOf("F18d", {}), kT + 700, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(countOf(out, EventType::NoShow)));
}

// ...but a pending schedule row whose deadline passed during an OUTAGE is dropped, not called a
// no-show: we were not watching, so "the bus never came" is not something we know (F18/F17).
static void test_tracker_pending_scheduled_dropped_on_outage_not_noshow(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F18e", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F18e", {schedArrival("uncertain", kT + 60)}), kT, true, true, out);
  for (transit::Epoch t = kT + 30; t <= kT + 700; t += 30) {
    tracker.observe(snapOf("F18e", {}), t, true, /*poll_ok=*/false, out);
  }
  tracker.observe(snapOf("F18e", {}), kT + 730, true, /*poll_ok=*/true, out);
  tracker.observe(snapOf("F18e", {}), kT + 760, true, /*poll_ok=*/true, out);

  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(countOf(out, EventType::NoShow)));
}

// ---- F19: the working set must not churn -------------------------------------------------

// The review's reproduction: nine trips, the same nine every poll, eight slots -- every poll
// evicted the entry it was about to need and re-emitted nine first-sighting `pred` rows, ten
// evictions deep. An unchanged feed must produce no rows at all after the first poll.
//
// Written against kMaxTrackedTripsPerStop + 1 rather than a literal nine, because what fixed F19
// is the ADMISSION POLICY (keep the soonest, stably), not the slot count: the trip that does not
// fit is refused once and stays refused, instead of displacing one that is already there. So this
// holds at 8 slots (0.3.1) exactly as it did at 12, and it is the test that would fail if a future
// change made the working set churn again at whatever the cap is then.
static void test_tracker_one_over_the_cap_does_not_churn(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F19a", "17", "0");
  std::vector<LogEvent> out;

  const int cap = (int)transit_stats::kMaxTrackedTripsPerStop;
  std::vector<transit::Arrival> feed;
  for (int i = 0; i < cap + 1; i++) {
    // Far enough out that no horizon milestone is crossed between the two polls, so anything that
    // shows up in round two is churn and nothing else.
    feed.push_back(liveArrival("T" + std::to_string(i), kT + 1000 + i * 120));
  }

  tracker.observe(snapOf("F19a", feed), kT, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cap),
                            static_cast<uint32_t>(countOf(out, EventType::Pred)));

  out.clear();
  tracker.observe(snapOf("F19a", feed), kT + 30, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(out.size()));

  StopCounters ctr;
  TEST_ASSERT_TRUE(tracker.getStopCounters("F19a", ctr));
  TEST_ASSERT_EQUAL_UINT32(0, ctr.evicted_trips);       // nothing already admitted was displaced
  TEST_ASSERT_EQUAL_UINT32(2, ctr.dropped_observations);  // the one that did not fit, once a poll
  TEST_ASSERT_EQUAL_UINT32(0, ctr.arrivals_seen);
  TEST_ASSERT_EQUAL_UINT32(0, ctr.ghosts_seen);
}

// Twenty trips into kMaxTrackedTripsPerStop slots: the SOONEST are kept, the rest are counted as dropped
// observations, and the set is stable -- no first sighting ever repeats, and the soonest arrivals
// are never the ones lost.
static void test_tracker_twenty_trip_feed_keeps_the_soonest(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F19b", "17", "0");
  std::vector<LogEvent> out;

  std::vector<transit::Arrival> feed;
  for (int i = 0; i < 20; i++) {
    feed.push_back(liveArrival("T" + std::to_string(i), kT + 1000 + i * 60));
  }

  tracker.observe(snapOf("F19b", feed), kT, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(transit_stats::kMaxTrackedTripsPerStop),
                            static_cast<uint32_t>(countOf(out, EventType::Pred)));
  for (int i = 0; i < 20; i++) {
    const std::string trip = "T" + std::to_string(i);
    const size_t preds = countOfTrip(out, EventType::Pred, trip);
    if (i < (int)transit_stats::kMaxTrackedTripsPerStop) {
      TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(preds));  // the soonest kMax..: admitted
    } else {
      TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(preds));  // the rest: ignored
    }
  }

  out.clear();
  tracker.observe(snapOf("F19b", feed), kT + 30, true, true, out);
  tracker.observe(snapOf("F19b", feed), kT + 60, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(out.size()));  // no churn, no re-sightings

  StopCounters ctr;
  TEST_ASSERT_TRUE(tracker.getStopCounters("F19b", ctr));
  TEST_ASSERT_EQUAL_UINT32(0, ctr.evicted_trips);
  // (20 - kMaxTrackedTripsPerStop) never admitted, on each of three polls.
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(3 * (20 - transit_stats::kMaxTrackedTripsPerStop)),
                            ctr.dropped_observations);
}

// A trip beyond the admission horizon is ignored rather than admitted-then-evicted, and is picked
// up normally once it is close enough to matter.
static void test_tracker_far_future_arrival_is_ignored_until_close(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F19c", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F19c", {liveArrival("FAR", kT + 5000)}), kT, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(out.size()));

  tracker.observe(snapOf("F19c", {liveArrival("FAR", kT + 5000)}), kT + 3000, true, true, out);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(countOfTrip(out, EventType::Pred, "FAR")));
}

// ---- F20: headways are computed in passage order, never slot order -----------------------

// The review's reproduction: two trips vanish in the same observation, the one predicted LATER
// sitting in the earlier slot. Emitting in slot order walked last_arrive_actual backwards and
// produced a -50 s headway, which the aggregator then counted (and could call bunching).
static void test_tracker_simultaneous_disappearances_sorted_by_passage_time(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F20a", "17", "0");
  std::vector<LogEvent> out;

  // Slot order A, B; passage order B (T+50), A (T+100).
  tracker.observe(snapOf("F20a", {liveArrival("A", kT + 100), liveArrival("B", kT + 50)}), kT, true,
                   true, out);
  out.clear();
  tracker.observe(snapOf("F20a", {}), kT + 150, true, true, out);
  tracker.observe(snapOf("F20a", {}), kT + 180, true, true, out);

  std::vector<LogEvent> arrives;
  for (const auto& ev : out) {
    if (ev.event == EventType::Arrive) arrives.push_back(ev);
  }
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(arrives.size()));
  TEST_ASSERT_EQUAL_STRING("B", arrives[0].trip.c_str());  // earlier inferred passage comes first
  TEST_ASSERT_EQUAL_INT64(kT + 50, *arrives[0].actual_ts);
  TEST_ASSERT_FALSE(arrives[0].headway_s.has_value());     // nothing before it at this stop
  TEST_ASSERT_EQUAL_STRING("A", arrives[1].trip.c_str());
  TEST_ASSERT_TRUE(arrives[1].headway_s.has_value());
  TEST_ASSERT_EQUAL_INT32(50, *arrives[1].headway_s);      // +50, never -50
  assertNoNegativeHeadways(out);
}

// Two trips whose inferred times coincide: the gap between them is not knowable, so no headway is
// written at all. A 0 would be averaged in downstream as a real, perfectly bunched pair.
static void test_tracker_equal_passage_times_write_no_headway(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F20b", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F20b", {liveArrival("X", kT + 100), liveArrival("Y", kT + 100)}), kT, true,
                   true, out);
  out.clear();
  tracker.observe(snapOf("F20b", {}), kT + 150, true, true, out);
  tracker.observe(snapOf("F20b", {}), kT + 180, true, true, out);

  TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(countOf(out, EventType::Arrive)));
  for (const auto& ev : out) {
    if (ev.event == EventType::Arrive) TEST_ASSERT_FALSE(ev.headway_s.has_value());
  }
  assertNoNegativeHeadways(out);
}

// Headway continuity does not survive an outage: the bus after the gap is not known to be the
// next one. The control case (same timings, no outage) shows the headway would otherwise be
// written, so this is the outage doing it and not some other filter.
static void test_tracker_headway_continuity_breaks_across_outage(void) {
  auto run = [](bool with_outage) {
    ArrivalTracker tracker;
    tracker.registerStop("F20c", "17", "0");
    std::vector<LogEvent> out;

    tracker.observe(snapOf("F20c", {liveArrival("X", kT)}), kT, true, true, out);
    tracker.observe(snapOf("F20c", {}), kT + 30, true, true, out);
    tracker.observe(snapOf("F20c", {}), kT + 60, true, true, out);  // X arrives, chain starts

    for (transit::Epoch t = kT + 90; t <= kT + 500; t += 30) {
      tracker.observe(snapOf("F20c", {}), t, true, /*poll_ok=*/!with_outage, out);
    }
    tracker.observe(snapOf("F20c", {liveArrival("Y", kT + 560)}), kT + 530, true, true, out);
    tracker.observe(snapOf("F20c", {}), kT + 590, true, true, out);
    tracker.observe(snapOf("F20c", {}), kT + 620, true, true, out);

    std::optional<int32_t> y_headway;
    for (const auto& ev : out) {
      if (ev.event == EventType::Arrive && ev.trip == "Y") y_headway = ev.headway_s;
    }
    assertNoNegativeHeadways(out);
    return y_headway;
  };

  TEST_ASSERT_TRUE(run(false).has_value());   // control: consecutive buses, headway written
  TEST_ASSERT_EQUAL_INT32(560, *run(false));
  TEST_ASSERT_FALSE(run(true).has_value());   // across an outage: no claim made
}

// An arrival on the far side of a local service-day boundary does not inherit the previous day's
// last arrival as its predecessor.
static void test_tracker_headway_continuity_breaks_at_service_day(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F20d", "17", "0");
  std::vector<LogEvent> out;

  // 2026-01-01 23:50 EST and 2026-01-02 00:05 EST: 15 minutes apart, different service days.
  const transit::Epoch late_night = 1767243600 + 23 * 3600 + 50 * 60;
  tracker.observe(snapOf("F20d", {liveArrival("N1", late_night)}), late_night, true, true, out);
  tracker.observe(snapOf("F20d", {}), late_night + 30, true, true, out);
  tracker.observe(snapOf("F20d", {}), late_night + 60, true, true, out);

  const transit::Epoch after_midnight = late_night + 900;
  tracker.observe(snapOf("F20d", {liveArrival("N2", after_midnight)}), after_midnight, true, true, out);
  tracker.observe(snapOf("F20d", {}), after_midnight + 30, true, true, out);
  tracker.observe(snapOf("F20d", {}), after_midnight + 60, true, true, out);

  for (const auto& ev : out) {
    if (ev.event == EventType::Arrive && ev.trip == "N2") TEST_ASSERT_FALSE(ev.headway_s.has_value());
  }
  assertNoNegativeHeadways(out);
}

// Re-registering a stop onto a different route/direction is a new service: nothing tracked for
// the old one carries over, headway included.
static void test_tracker_reregistration_breaks_continuity(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F20e", "17", "0");
  std::vector<LogEvent> out;

  tracker.observe(snapOf("F20e", {liveArrival("P", kT)}), kT, true, true, out);
  tracker.observe(snapOf("F20e", {}), kT + 30, true, true, out);
  tracker.observe(snapOf("F20e", {}), kT + 60, true, true, out);

  tracker.registerStop("F20e", "T4", "1");  // user pointed this stop at another route

  tracker.observe(snapOf("F20e", {liveArrival("Q", kT + 300)}), kT + 200, true, true, out);
  tracker.observe(snapOf("F20e", {}), kT + 330, true, true, out);
  tracker.observe(snapOf("F20e", {}), kT + 360, true, true, out);

  for (const auto& ev : out) {
    if (ev.event == EventType::Arrive && ev.trip == "Q") TEST_ASSERT_FALSE(ev.headway_s.has_value());
  }
}

// The aggregator defends itself too: a negative headway in an old log row (or a corrupt one) is
// ignored rather than averaged in or counted as bunching.
static void test_aggregator_ignores_negative_and_overlong_headways(void) {
  StatsAggregator agg("NEG", 0, 100000, &trivialHourWeekday);
  auto feed = [&](transit::Epoch ts, const std::string& trip, transit::Epoch scheduled,
                   std::optional<int32_t> headway_s) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = "NEG";
    ev.trip = trip;
    ev.actual_ts = ts;
    ev.scheduled_ts = scheduled;
    ev.headway_s = headway_s;
    const std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
  };

  feed(28800, "S0", 28800, std::nullopt);  // seed, establishes prev_scheduled
  feed(28810, "S1", 29400, -50);           // the F20 defect's signature value
  feed(28820, "S2", 30000, 0);             // ambiguous, not a real gap
  feed(28830, "S3", 30600, 4 * 3600);      // an overnight hole, not a wait

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);
  TEST_ASSERT_EQUAL_UINT16(0, doc["headway"]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT16(0, doc["headway"]["bunched"].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT16(0, doc["wait_by_hour"][8]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT32(4, doc["samples"].as<uint32_t>());  // still counted as arrivals
}

// ---- F21: unknown lateness is not a late bus ---------------------------------------------

// The review's reproduction: one on-time arrival plus one arrival with no lateness reading read
// as "50% on time". The unknown sample belongs in neither the numerator nor the denominator.
static void test_aggregator_on_time_pct_over_known_lateness_only(void) {
  // Mixed: one known on-time + one unknown is 100% of what we know, not 50% of everything.
  {
    StatsAggregator agg("OT", 0, 100000, &trivialHourWeekday);
    LogEvent k;
    k.ts = 28800; k.event = EventType::Arrive; k.stop_key = "OT"; k.trip = "K";
    k.actual_ts = 28800; k.late_min = 2;
    LogEvent u;
    u.ts = 28900; u.event = EventType::Arrive; u.stop_key = "OT"; u.trip = "U"; u.actual_ts = 28900;
    for (const LogEvent* ev : {&k, &u}) {
      const std::string line = transit_stats::toCsv(*ev);
      agg.feedLine(line.c_str(), line.size());
    }
    ArduinoJson::JsonDocument doc;
    agg.toJson(doc);
    TEST_ASSERT_EQUAL_UINT32(2, doc["samples"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(1, doc["late_known"].as<uint32_t>());
    TEST_ASSERT_FALSE(doc["on_time_pct"].isNull());
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 100.0f, doc["on_time_pct"].as<float>());
    TEST_ASSERT_TRUE(agg.hasOnTime());
  }

  // All-unknown: two arrivals, nothing known about lateness -> null, not 0.
  {
    StatsAggregator agg("OT", 0, 100000, &trivialHourWeekday);
    for (int i = 0; i < 2; i++) {
      LogEvent ev;
      ev.ts = 28800 + i; ev.event = EventType::Arrive; ev.stop_key = "OT";
      ev.trip = "U" + std::to_string(i); ev.actual_ts = ev.ts;
      const std::string line = transit_stats::toCsv(ev);
      agg.feedLine(line.c_str(), line.size());
    }
    ArduinoJson::JsonDocument doc;
    agg.toJson(doc);
    TEST_ASSERT_EQUAL_UINT32(2, doc["samples"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(0, doc["late_known"].as<uint32_t>());
    TEST_ASSERT_TRUE(doc["on_time_pct"].isNull());
    TEST_ASSERT_TRUE(doc["mean_late_min"].isNull());
    TEST_ASSERT_FALSE(agg.hasOnTime());
  }

  // Empty window: distinct again -- zero samples, and still no percentage to report.
  {
    StatsAggregator agg("OT", 0, 100000, &trivialHourWeekday);
    ArduinoJson::JsonDocument doc;
    agg.toJson(doc);
    TEST_ASSERT_EQUAL_UINT32(0, doc["samples"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(0, doc["late_known"].as<uint32_t>());
    TEST_ASSERT_TRUE(doc["on_time_pct"].isNull());
  }

}

// ---- F22: inferred arrivals say so --------------------------------------------------------

static void test_tracker_arrive_rows_carry_inference_method(void) {
  ArrivalTracker tracker;
  tracker.registerStop("F22", "17", "0");
  std::vector<LogEvent> out;

  // Vanishes near its prediction -> "inferred".
  tracker.observe(snapOf("F22", {liveArrival("I", kT + 60)}), kT, true, true, out);
  tracker.observe(snapOf("F22", {}), kT + 90, true, true, out);
  tracker.observe(snapOf("F22", {}), kT + 120, true, true, out);

  // Still being predicted long after it should have arrived, then vanishes -> "late-vanish".
  tracker.observe(snapOf("F22", {liveArrival("LV", kT + 200)}), kT + 200, true, true, out);
  tracker.observe(snapOf("F22", {liveArrival("LV", kT + 200)}), kT + 600, true, true, out);
  tracker.observe(snapOf("F22", {}), kT + 630, true, true, out);
  tracker.observe(snapOf("F22", {}), kT + 660, true, true, out);

  bool saw_inferred = false, saw_late_vanish = false;
  for (const auto& ev : out) {
    if (ev.event != EventType::Arrive) continue;
    if (ev.trip == "I") {
      saw_inferred = ev.note == transit_stats::kNoteInferred;
      TEST_ASSERT_TRUE(ev.horizon_s.has_value());   // horizon left when we lost sight of it
      TEST_ASSERT_EQUAL_INT32(-30, *ev.horizon_s);
    }
    if (ev.trip == "LV") saw_late_vanish = ev.note == transit_stats::kNoteLateVanish;
    TEST_ASSERT_TRUE(transit_stats::isInferenceNote(ev.note));  // never a bare, unexplained arrive
  }
  TEST_ASSERT_TRUE(saw_inferred);
  TEST_ASSERT_TRUE(saw_late_vanish);
}

static void test_aggregator_counts_inferred_arrivals(void) {
  StatsAggregator agg("INF", 0, 100000, &trivialHourWeekday);
  auto arrive = [&](transit::Epoch ts, const std::string& trip, const char* note) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = "INF";
    ev.trip = trip;
    ev.actual_ts = ts;
    ev.note = note;
    const std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
  };
  arrive(1000, "A", transit_stats::kNoteInferred);
  arrive(2000, "B", transit_stats::kNoteUnobserved);
  arrive(3000, "C", transit_stats::kNoteLateVanish);
  arrive(4000, "D", "");  // a pre-2026-09-15 row: inferred too, but does not say so

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);
  TEST_ASSERT_EQUAL_UINT32(4, doc["samples"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(3, doc["inferred"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(1, doc["unobserved"].as<uint32_t>());
}

// ---- F23: outage totals include the detection delay and unfinished intervals --------------

// Each case is one outage interval placed differently relative to a one-day window; the reported
// figure must be the OVERLAP with that window. The old code counted only start/end pairs whose
// rows both fell inside the window, and timestamped the start at detection time, so a 600 s
// outage was logged as 299 s and an ongoing one as nothing at all.
static void test_aggregator_outage_overlap_variants(void) {
  const transit::Epoch w0 = 1767243600;               // 2026-01-01 00:00 EST
  const transit::Epoch w1 = w0 + 86400;

  auto outageMinutes = [&](transit::Epoch start, std::optional<transit::Epoch> end) {
    StatsAggregator agg("OUT", w0, w1, &trivialHourWeekday);
    auto feed = [&](transit::Epoch ts, const char* note) {
      LogEvent ev;
      ev.ts = ts;
      ev.event = EventType::Outage;
      ev.stop_key = "OUT";
      ev.note = note;
      const std::string line = transit_stats::toCsv(ev);
      agg.feedLine(line.c_str(), line.size());
    };
    feed(start, "");
    if (end) feed(*end, "end");
    ArduinoJson::JsonDocument doc;
    agg.toJson(doc);
    return doc["outage_min"].as<int32_t>();
  };

  // Closed, entirely inside the window: the full 10 minutes, detection delay included.
  TEST_ASSERT_EQUAL_INT32(10, outageMinutes(w0 + 3600, w0 + 3600 + 600));
  // Ongoing (no `end` row at all): runs to the end of the window, not zero.
  TEST_ASSERT_EQUAL_INT32(100, outageMinutes(w1 - 6000, std::nullopt));
  // Started before the window: only the part inside it counts.
  TEST_ASSERT_EQUAL_INT32(10, outageMinutes(w0 - 3600, w0 + 600));
  // Ends after the window: likewise.
  TEST_ASSERT_EQUAL_INT32(10, outageMinutes(w1 - 600, w1 + 3600));
  // Spans the whole window (a month-crossing outage): the whole window is lost.
  TEST_ASSERT_EQUAL_INT32(1440, outageMinutes(w0 - 100000, w1 + 100000));
  // Entirely before the window: nothing.
  TEST_ASSERT_EQUAL_INT32(0, outageMinutes(w0 - 7200, w0 - 3600));

  // Coverage follows from the same arithmetic and is reported alongside it.
  {
    StatsAggregator agg("OUT", w0, w1, &trivialHourWeekday);
    LogEvent ev;
    ev.ts = w0 + 3600;
    ev.event = EventType::Outage;
    ev.stop_key = "OUT";
    std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
    ev.ts = w0 + 3600 + 8640;  // 10% of a day
    ev.note = "end";
    line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());

    ArduinoJson::JsonDocument doc;
    agg.toJson(doc);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, doc["coverage"].as<float>());
  }
}

// ---- F24/F25: one header, one bounded record shape ---------------------------------------

static void test_csv_schema_version_and_single_header(void) {
  TEST_ASSERT_EQUAL_INT(3, transit_stats::csvSchemaVersion());

  const std::string header = transit_stats::csvHeader();
  TEST_ASSERT_TRUE(header.find("temp_c") != std::string::npos);  // v3 renames temp -> temp_c
  TEST_ASSERT_TRUE(header.find(",temp,") == std::string::npos);

  // 21 columns in the header, matching what toCsv() writes -- this is what the SD logger used to
  // get wrong, writing a 14-column v1 header above 21-column rows (F24).
  size_t header_commas = 0;
  for (char c : header) header_commas += (c == ',');
  LogEvent ev;
  ev.ts = 1;
  ev.stop_key = "S";
  const std::string row = transit_stats::toCsv(ev);
  size_t row_commas = 0;
  for (char c : row) row_commas += (c == ',');
  TEST_ASSERT_EQUAL_UINT32(20, static_cast<uint32_t>(header_commas));
  TEST_ASSERT_EQUAL_UINT32(header_commas, static_cast<uint32_t>(row_commas));

  // Both the v1 and the current header row are unparsable, so a mixed-schema file's header lines
  // are skipped by the ordinary "skip what does not parse" rule.
  LogEvent parsed;
  TEST_ASSERT_FALSE(transit_stats::fromCsv(header.c_str(), header.size(), parsed));
  const std::string v1h = transit_stats::csvHeaderV1();
  TEST_ASSERT_FALSE(transit_stats::fromCsv(v1h.c_str(), v1h.size(), parsed));
}

static void test_normalize_csv_line_to_v3(void) {
  // A v1 (14-column) row gains the seven missing columns as empty fields.
  const char* v1 = "1757800000,arrive,17-21332,17,0,3667,7477,,1757800010,1757800012,-3,,900,";
  std::string out;
  TEST_ASSERT_TRUE(transit_stats::normalizeCsvLine(v1, std::string(v1).size(), out));
  size_t commas = 0;
  for (char c : out) commas += (c == ',');
  TEST_ASSERT_EQUAL_UINT32(20, static_cast<uint32_t>(commas));

  LogEvent parsed;
  TEST_ASSERT_TRUE(transit_stats::fromCsv(out.c_str(), out.size(), parsed));
  TEST_ASSERT_TRUE(parsed.event == EventType::Arrive);
  TEST_ASSERT_EQUAL_STRING("17-21332", parsed.stop_key.c_str());
  // A negative NUMBER must stay a number: neutralising formulas must not touch numeric columns.
  TEST_ASSERT_TRUE(parsed.late_min.has_value());
  TEST_ASSERT_EQUAL_INT32(-3, *parsed.late_min);

  // A 21-column row passes through with its columns intact.
  LogEvent ev;
  ev.ts = 1757900000;
  ev.event = EventType::Bike;
  ev.stop_key = "indego-3468";
  ev.note = "Snyder & Dorrance";
  ev.bikes = 4;
  const std::string v3 = transit_stats::toCsv(ev);
  TEST_ASSERT_TRUE(transit_stats::normalizeCsvLine(v3.c_str(), v3.size(), out));
  TEST_ASSERT_TRUE(transit_stats::fromCsv(out.c_str(), out.size(), parsed));
  TEST_ASSERT_EQUAL_STRING("Snyder & Dorrance", parsed.note.c_str());
  TEST_ASSERT_EQUAL_INT32(4, *parsed.bikes);

  // Spreadsheet safety: a text field that would be read as a formula is prefixed with a quote in
  // the EXPORT only. The stored row keeps the exact bytes the device wrote.
  LogEvent nasty;
  nasty.ts = 100;
  nasty.event = EventType::Arrive;
  nasty.stop_key = "S";
  nasty.trip = "=HYPERLINK(\"http://x\")";
  nasty.note = "@SUM(A1:A9)";
  const std::string stored = transit_stats::toCsv(nasty);
  TEST_ASSERT_TRUE(stored.find("'") == std::string::npos);  // never in the log file itself
  TEST_ASSERT_TRUE(transit_stats::normalizeCsvLine(stored.c_str(), stored.size(), out));
  TEST_ASSERT_TRUE(transit_stats::fromCsv(out.c_str(), out.size(), parsed));
  TEST_ASSERT_EQUAL_CHAR('\'', parsed.trip[0]);
  TEST_ASSERT_EQUAL_CHAR('\'', parsed.note[0]);

  // Header rows, blank lines and garbage are skipped, not turned into rows.
  const std::string header = transit_stats::csvHeader();
  TEST_ASSERT_FALSE(transit_stats::normalizeCsvLine(header.c_str(), header.size(), out));
  TEST_ASSERT_FALSE(transit_stats::normalizeCsvLine("", 0, out));
  TEST_ASSERT_FALSE(transit_stats::normalizeCsvLine("nonsense", 8, out));
}

// F25: whatever goes into a LogEvent, the record that comes out is one line, no CR/LF inside a
// field, and never longer than kMaxCsvLineBytes -- which is what makes the app's fixed 512-byte,
// split-on-newline SD reader correct instead of merely usually-correct.
static void test_csv_records_are_bounded_and_single_line(void) {
  LogEvent ev;
  ev.ts = 1757900000;
  ev.event = EventType::Arrive;
  ev.stop_key = std::string(80, 'S');            // absurd, but must not overflow the record
  ev.trip = std::string(200, '9');
  ev.vehicle = std::string(200, '7');
  ev.note = "line one\r\nline two\ttabbed" + std::string(400, 'x');

  const std::string line = transit_stats::toCsv(ev);
  TEST_ASSERT_TRUE(line.size() <= transit_stats::kMaxCsvLineBytes);
  TEST_ASSERT_TRUE(line.find('\n') == std::string::npos);
  TEST_ASSERT_TRUE(line.find('\r') == std::string::npos);

  LogEvent parsed;
  TEST_ASSERT_TRUE(transit_stats::fromCsv(line.c_str(), line.size(), parsed));
  TEST_ASSERT_TRUE(parsed.trip.size() <= transit_stats::kMaxCsvTextChars);
  TEST_ASSERT_TRUE(parsed.stop_key.size() <= transit_stats::kMaxCsvIdChars);
  TEST_ASSERT_EQUAL_STRING("line one  line two tabbed", parsed.note.substr(0, 25).c_str());

  // Commas and quotes still round-trip exactly (they are quoted, not mangled).
  LogEvent q;
  q.ts = 1;
  q.event = EventType::Arrive;
  q.stop_key = "S";
  q.note = "operator said, \"late, sorry\"";
  const std::string qline = transit_stats::toCsv(q);
  TEST_ASSERT_TRUE(transit_stats::fromCsv(qline.c_str(), qline.size(), parsed));
  TEST_ASSERT_EQUAL_STRING(q.note.c_str(), parsed.note.c_str());
}

// A blank line or a corrupt/over-long record in the middle of a month's log must cost exactly
// that record -- never the rest of the file.
static void test_reader_skips_blank_and_overlong_records(void) {
  StatsAggregator agg("SKIP", 0, 100000, &trivialHourWeekday);
  auto arriveLine = [](transit::Epoch ts, const std::string& trip) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = "SKIP";
    ev.trip = trip;
    ev.actual_ts = ts;
    ev.late_min = 1;
    return transit_stats::toCsv(ev);
  };

  const std::vector<std::string> file = {
      arriveLine(1000, "A"),
      "",                                     // blank line mid-file
      "   ",                                  // whitespace-only line
      std::string(700, 'x'),                  // over-long garbage, past kMaxCsvLineBytes
      "1,arrive,SKIP,17,0,T,,,,,,,,",         // v1 shape but truncated payload: still parses
      arriveLine(2000, "B"),
  };
  for (const auto& l : file) agg.feedLine(l.c_str(), l.size());

  LogEvent probe;
  TEST_ASSERT_FALSE(transit_stats::fromCsv("", 0, probe));
  TEST_ASSERT_FALSE(transit_stats::fromCsv(file[3].c_str(), file[3].size(), probe));

  // A (v1) row at ts=1 plus the two real arrivals: the records after the damage survived.
  TEST_ASSERT_EQUAL_UINT32(3, agg.samples());
  TEST_ASSERT_EQUAL_UINT32(2, agg.lateKnown());
}

// ---- F28: the stops you have configured now own the overview slots -----------------------

// The review's reproduction: eight historical stop_keys fill the eight first-seen slots, so the
// two stops the user actually has configured are silently missing from the overview page.
static void test_overview_reserved_stops_survive_a_history_of_others(void) {
  const std::vector<std::string> current = {"17-21332", "17-21297"};
  OverviewAggregator agg(0, 200000, current, {}, &trivialHourWeekday);

  auto arrive = [&](const std::string& stop, transit::Epoch ts, int32_t late_min) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Arrive;
    ev.stop_key = stop;
    ev.trip = "T";
    ev.actual_ts = ts;
    ev.late_min = late_min;
    const std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
  };

  // Eight older stops, all seen BEFORE the current ones, exactly as a month of history would be.
  for (int i = 1; i <= 8; i++) arrive("OLD" + std::to_string(i), 1000 + i, 4);
  arrive("17-21332", 9000, 2);
  arrive("17-21297", 9100, 9);

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);

  ArduinoJson::JsonArray stops = doc["stops"];
  TEST_ASSERT_EQUAL_INT(8, static_cast<int>(stops.size()));
  // The configured stops come first and carry their own numbers, not someone else's.
  TEST_ASSERT_EQUAL_STRING("17-21332", stops[0]["stop"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT32(1, stops[0]["samples"].as<uint32_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 100.0f, stops[0]["on_time_pct"].as<float>());
  TEST_ASSERT_EQUAL_STRING("17-21297", stops[1]["stop"].as<const char*>());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, stops[1]["on_time_pct"].as<float>());  // 9 min late
  // Six of the eight older stops fit in the remaining slots; the other two are disclosed.
  TEST_ASSERT_EQUAL_STRING("OLD1", stops[2]["stop"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT32(2, doc["excluded_stops"].as<uint32_t>());
  // Top-level `samples` is the total across the stops actually LISTED (8 of the 10 arrivals);
  // the two rows belonging to excluded stops are accounted for by excluded_stops, not silently
  // folded into a total that would not match the rows above it.
  TEST_ASSERT_EQUAL_UINT32(8, doc["samples"].as<uint32_t>());
}

// A reserved stop with no rows at all in the window still appears -- "just added, no data yet" is
// a real answer, and dropping the row would make a new stop look like it does not exist.
static void test_overview_reserved_stop_with_no_rows_still_appears(void) {
  OverviewAggregator agg(0, 200000, {"17-NEW"}, {}, &trivialHourWeekday);
  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);
  ArduinoJson::JsonArray stops = doc["stops"];
  TEST_ASSERT_EQUAL_INT(1, static_cast<int>(stops.size()));
  TEST_ASSERT_EQUAL_STRING("17-NEW", stops[0]["stop"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT32(0, stops[0]["samples"].as<uint32_t>());
  TEST_ASSERT_TRUE(stops[0]["on_time_pct"].isNull());  // no data, not "0% on time"
}

// The 3-to-4 bike-station transition: the user swaps one of three configured Indego stations for
// a different one, so the window contains four. The three configured now win the slots.
static void test_overview_bike_station_three_to_four_transition(void) {
  const std::vector<std::string> current = {"indego-3468", "indego-3101", "indego-3999"};
  OverviewAggregator agg(0, 200000, {}, current, &trivialHourWeekday);

  auto bike = [&](const std::string& station, transit::Epoch ts, int32_t bikes) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Bike;
    ev.stop_key = station;
    ev.note = station + " name";
    ev.bikes = bikes;
    const std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
  };

  bike("indego-3007", 28800, 1);  // the station the user removed, seen first in the window
  bike("indego-3468", 28810, 4);
  bike("indego-3101", 28820, 5);
  bike("indego-3999", 28830, 6);

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);
  ArduinoJson::JsonArray bikes = doc["bikes"];
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(bikes.size()));
  TEST_ASSERT_EQUAL_STRING("indego-3468", bikes[0]["station"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("indego-3101", bikes[1]["station"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("indego-3999", bikes[2]["station"].as<const char*>());
  TEST_ASSERT_EQUAL_UINT32(1, doc["bikes"][0]["samples"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(1, doc["excluded_bikes"].as<uint32_t>());
}

// Per-stop outage accounting in the overview follows the same overlap rule as the per-stop
// endpoint, including an outage that is still open at the end of the window (F23).
static void test_overview_outage_and_coverage_per_stop(void) {
  const transit::Epoch w0 = 0, w1 = 86400;
  OverviewAggregator agg(w0, w1, {"S"}, {}, &trivialHourWeekday);
  auto outage = [&](transit::Epoch ts, const char* note) {
    LogEvent ev;
    ev.ts = ts;
    ev.event = EventType::Outage;
    ev.stop_key = "S";
    ev.note = note;
    const std::string line = transit_stats::toCsv(ev);
    agg.feedLine(line.c_str(), line.size());
  };

  outage(-3600, "");        // started before the window
  outage(600, "end");       // 600 s of overlap
  outage(w1 - 8640, "");    // still open at the end of the window: 8640 s more

  ArduinoJson::JsonDocument doc;
  agg.toJson(doc);
  TEST_ASSERT_EQUAL_INT32((600 + 8640) / 60, doc["stops"][0]["outage_min"].as<int32_t>());
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 1.0f - 9240.0f / 86400.0f,
                            doc["stops"][0]["coverage"].as<float>());
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_csv_round_trip_full);
  RUN_TEST(test_csv_round_trip_empty_optionals_are_empty_not_zero);
  RUN_TEST(test_csv_header_line_is_rejected);
  RUN_TEST(test_csv_round_trip_v2_full);
  RUN_TEST(test_csv_round_trip_bike_event);
  RUN_TEST(test_csv_round_trip_v1_14_columns_still_parses);
  RUN_TEST(test_csv_wrong_column_count_is_rejected);
  RUN_TEST(test_seats_token_mapping);
  RUN_TEST(test_seats_level_mapping);

  RUN_TEST(test_america_new_york_local_time);

  RUN_TEST(test_tracker_pred_horizons_and_arrive_with_headway);
  RUN_TEST(test_tracker_ghost);
  RUN_TEST(test_tracker_noshow_vs_outage);
  RUN_TEST(test_tracker_outage_start_end);
  RUN_TEST(test_tracker_memory_bound_eviction);
  RUN_TEST(test_tracker_seats_on_pred_and_arrive);

  RUN_TEST(test_aggregator_synthetic_month);
  RUN_TEST(test_aggregator_crowding_and_wait_by_hour);
  RUN_TEST(test_summary_from_aggregator);

  RUN_TEST(test_overview_aggregator_stops_and_bikes);
  RUN_TEST(test_overview_aggregator_capacity_ignores_overflow);

  RUN_TEST(test_stats_aggregator_size_budget);

  RUN_TEST(test_months_in_window_single_day_one_month);
  RUN_TEST(test_months_in_window_spans_month_boundary);
  RUN_TEST(test_months_in_window_spans_year_boundary);
  RUN_TEST(test_months_in_window_many_months);
  RUN_TEST(test_months_in_window_empty_when_end_not_after_start);
  RUN_TEST(test_months_in_window_across_dst_spring_forward);

  // 2026-09-15 measurement-defect review (F17-F25, F28).
  RUN_TEST(test_tracker_failed_poll_creates_no_arrival);
  RUN_TEST(test_tracker_outage_recovery_closes_unobserved_once);
  RUN_TEST(test_tracker_outage_recovery_no_double_count);
  RUN_TEST(test_tracker_inference_needs_two_successful_misses);

  RUN_TEST(test_tracker_scheduled_then_live_same_id_no_noshow);
  RUN_TEST(test_tracker_scheduled_then_live_different_ids_same_scheduled_time);
  RUN_TEST(test_tracker_never_observed_scheduled_still_noshow);
  RUN_TEST(test_tracker_pending_scheduled_dropped_on_outage_not_noshow);

  RUN_TEST(test_tracker_one_over_the_cap_does_not_churn);
  RUN_TEST(test_tracker_twenty_trip_feed_keeps_the_soonest);
  RUN_TEST(test_tracker_far_future_arrival_is_ignored_until_close);

  RUN_TEST(test_tracker_simultaneous_disappearances_sorted_by_passage_time);
  RUN_TEST(test_tracker_equal_passage_times_write_no_headway);
  RUN_TEST(test_tracker_headway_continuity_breaks_across_outage);
  RUN_TEST(test_tracker_headway_continuity_breaks_at_service_day);
  RUN_TEST(test_tracker_reregistration_breaks_continuity);
  RUN_TEST(test_aggregator_ignores_negative_and_overlong_headways);

  RUN_TEST(test_aggregator_on_time_pct_over_known_lateness_only);

  RUN_TEST(test_tracker_arrive_rows_carry_inference_method);
  RUN_TEST(test_aggregator_counts_inferred_arrivals);

  RUN_TEST(test_aggregator_outage_overlap_variants);

  RUN_TEST(test_csv_schema_version_and_single_header);
  RUN_TEST(test_normalize_csv_line_to_v3);
  RUN_TEST(test_csv_records_are_bounded_and_single_line);
  RUN_TEST(test_reader_skips_blank_and_overlong_records);

  RUN_TEST(test_overview_reserved_stops_survive_a_history_of_others);
  RUN_TEST(test_overview_reserved_stop_with_no_rows_still_appears);
  RUN_TEST(test_overview_bike_station_three_to_four_transition);
  RUN_TEST(test_overview_outage_and_coverage_per_stop);

  return UNITY_END();
}
