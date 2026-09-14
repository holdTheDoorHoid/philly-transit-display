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
#include "transit_stats/summary.h"
#include "transit_stats/tracker.h"

using transit_stats::ArrivalTracker;
using transit_stats::EventType;
using transit_stats::LogEvent;
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
  //               late_min,horizon_s,headway_s,note
  TEST_ASSERT_EQUAL_STRING("42,ghost,S,17,0,T1,,,,,,,,", csv.c_str());

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

  const transit::Epoch t0 = 2000000000;

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
    transit::StopSnapshot snap;
    snap.key = "S1";  // A no longer present
    tracker.observe(snap, t0 + 30, true, true, out);
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
// when polling recovers; no duplicate start while still failing.
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
  TEST_ASSERT_EQUAL_INT64(t0 + 310, outages[0].ts);
  TEST_ASSERT_EQUAL_STRING("end", outages[1].note.c_str());
  TEST_ASSERT_EQUAL_INT64(t0 + 400, outages[1].ts);
}

// (e) Memory bound eviction: more than kMaxTrackedTripsPerStop distinct trips at one stop, and
// more than kMaxTrackedStops distinct stops, both evict the least-recently-touched entry and
// increment the exposed counters rather than growing without bound.
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
    TEST_ASSERT_EQUAL_UINT32(1, ctr.evicted_trips);
  }
  {
    ArrivalTracker tracker;
    std::vector<LogEvent> out;
    for (int i = 1; i <= 9; i++) {  // one more than kMaxTrackedStops (8)
      transit::StopSnapshot snap;
      snap.key = "K" + std::to_string(i);
      tracker.observe(snap, 7000000000 + i, true, true, out);
    }
    TEST_ASSERT_EQUAL_UINT8(8, tracker.trackedStopCount());
    TEST_ASSERT_TRUE(tracker.evictedStopCount() >= 1);

    StopCounters ctr;
    TEST_ASSERT_FALSE(tracker.getStopCounters("K1", ctr));  // least-recently-touched, evicted
    TEST_ASSERT_TRUE(tracker.getStopCounters("K9", ctr));   // most recent, retained
  }
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

  const double expected_on_time_pct =
      m.expected_samples ? 100.0 * m.expected_on_time / m.expected_samples : 0.0;
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

  ArduinoJson::JsonArray prediction = doc["prediction"];
  TEST_ASSERT_EQUAL_INT(4, static_cast<int>(prediction.size()));

  TEST_ASSERT_EQUAL_INT32(120, prediction[0]["horizon_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(8, prediction[0]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(30, prediction[0]["mae_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(30, prediction[0]["bias_s"].as<int32_t>());

  TEST_ASSERT_EQUAL_INT32(300, prediction[1]["horizon_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(6, prediction[1]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(20, prediction[1]["mae_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(-20, prediction[1]["bias_s"].as<int32_t>());

  TEST_ASSERT_EQUAL_INT32(600, prediction[2]["horizon_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(4, prediction[2]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(30, prediction[2]["mae_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(20, prediction[2]["bias_s"].as<int32_t>());

  TEST_ASSERT_EQUAL_INT32(900, prediction[3]["horizon_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_UINT16(2, prediction[3]["n"].as<uint16_t>());
  TEST_ASSERT_EQUAL_INT32(15, prediction[3]["mae_s"].as<int32_t>());
  TEST_ASSERT_EQUAL_INT32(15, prediction[3]["bias_s"].as<int32_t>());
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
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, s.on_time_pct);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, s.mean_late_min);
  TEST_ASSERT_EQUAL_UINT32(0, s.ghosts);
}

// =============================================================================================
// Memory budget (DESIGN.md §9.3: "all computed in one streaming pass with fixed-size
// accumulators (< 8 KB)").
// =============================================================================================

static void test_stats_aggregator_size_budget(void) {
  printf("sizeof(transit_stats::StatsAggregator) = %zu bytes\n", sizeof(StatsAggregator));
  printf("sizeof(transit_stats::ArrivalTracker)   = %zu bytes\n", sizeof(ArrivalTracker));
  TEST_ASSERT_TRUE(sizeof(StatsAggregator) < 8192);
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

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_csv_round_trip_full);
  RUN_TEST(test_csv_round_trip_empty_optionals_are_empty_not_zero);
  RUN_TEST(test_csv_header_line_is_rejected);

  RUN_TEST(test_america_new_york_local_time);

  RUN_TEST(test_tracker_pred_horizons_and_arrive_with_headway);
  RUN_TEST(test_tracker_ghost);
  RUN_TEST(test_tracker_noshow_vs_outage);
  RUN_TEST(test_tracker_outage_start_end);
  RUN_TEST(test_tracker_memory_bound_eviction);

  RUN_TEST(test_aggregator_synthetic_month);
  RUN_TEST(test_summary_from_aggregator);
  RUN_TEST(test_stats_aggregator_size_budget);

  RUN_TEST(test_months_in_window_single_day_one_month);
  RUN_TEST(test_months_in_window_spans_month_boundary);
  RUN_TEST(test_months_in_window_spans_year_boundary);
  RUN_TEST(test_months_in_window_many_months);
  RUN_TEST(test_months_in_window_empty_when_end_not_after_start);
  RUN_TEST(test_months_in_window_across_dst_spring_forward);

  return UNITY_END();
}
