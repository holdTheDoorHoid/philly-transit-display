// StatsAggregator: streams the SD-card CSV log for one stop and produces the DESIGN.md §9.3
// JSON shape, using only fixed-size accumulators (never loading the log into memory).
// Arduino-independent except for ArduinoJson (the project's one approved JSON library).
#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "transit_core/model.h"
#include "transit_stats/events.h"

namespace transit_stats {

// Converts a UTC unix-seconds epoch to local hour-of-day (0-23) and weekday (0=Sunday .. 6=
// Saturday, i.e. struct tm's tm_wday convention) for America/New_York, applying the current US
// DST rule (starts 2nd Sunday of March at 02:00 local, ends 1st Sunday of November at 02:00
// local) computed directly from the calendar (Howard Hinnant's civil-calendar algorithms), with
// no dependency on an OS timezone database. Deterministic, so tests don't depend on the host's
// or device's local time configuration.
void americaNewYorkLocalHourWeekday(transit::Epoch utc, int& hour_local, int& weekday_local);

// Injectable epoch -> (local hour, local weekday) conversion, so StatsAggregator's tests can
// supply a trivial/deterministic function instead of (or in addition to) the real one above.
using TzHourWeekdayFn = void (*)(transit::Epoch utc, int& hour_local, int& weekday_local);

// The four prediction-accuracy horizon buckets, in ascending order. A `pred` LogEvent's
// horizon_s is assigned to the smallest bucket that is >= horizon_s (see aggregate.cpp); this
// array's order is part of that contract and also fixes the order of the "prediction" array in
// toJson().
constexpr int32_t kHorizonBuckets[4] = {120, 300, 600, 900};

// Fixed-size lateness accumulator for one hour-of-day or weekday bin (DESIGN §9.2).
// Only samples with a known late_min are counted (mean/percentiles are undefined without it).
// hist[] spans late_min in [-10, 60] minutes inclusive, clamped at the edges: index = late_min +
// 10. p50/p90 are approximated from this histogram (DESIGN §9.2 explicitly allows this).
struct LatenessBucket {
  uint16_t n = 0;
  int32_t sum_late_min = 0;
  uint16_t hist[71] = {};
};

// Headway accumulator (DESIGN §9.2). bunched = actual/scheduled < 0.40; gapped = > 1.75.
// ratio_hist bin i covers [i*0.1, (i+1)*0.1); bin 19 catches everything >= 1.9.
struct HeadwayStats {
  uint16_t n = 0;
  uint16_t bunched = 0;
  uint16_t gapped = 0;
  uint16_t ratio_hist[20] = {};
};

// Prediction-accuracy accumulator for one horizon bucket. error = predicted_ts - actual_ts.
struct PredictionBucket {
  uint16_t n = 0;
  int32_t sum_abs_err_s = 0;
  int32_t sum_err_s = 0;
};

// Crowding accumulator for one hour-of-day or weekday bin (log schema v2, DESIGN.md §9.3). Only
// `arrive` rows with a known `seats` token contribute. dist[] follows seatsLevel()'s order:
// empty, open, few, standing, packed, full.
struct CrowdingBucket {
  uint16_t n = 0;
  uint32_t sum_level = 0;  // sum of seatsLevel() values, for the 1-decimal mean
  uint16_t dist[6] = {};
};

// Wait/gap accumulator for one hour-of-day bin (DESIGN.md §9.3 "wait_by_hour"). n/mean_gap_s/
// max_gap_s come from `arrive` rows with a known, positive headway_s; ghost/noshow are simple
// per-hour counts of those event rows. Bucketed by the local hour of the row's own `ts` (not
// actual_ts), per DESIGN.
struct WaitBucket {
  uint16_t n = 0;
  uint32_t sum_gap_s = 0;
  uint32_t max_gap_s = 0;
  uint16_t ghost = 0;
  uint16_t noshow = 0;
};

constexpr size_t kMaxPendingPredTrips = 64;

// One `pred` row's state, waiting to be matched to a later `arrive` row for the same trip.
// `trip` is a fixed-size, truncated copy of LogEvent::trip (real SEPTA trip ids are a handful of
// digits; anything past 15 bytes is truncated, which can only cause a missed/incorrect match for
// that one unusually-long id, never a crash or unbounded allocation).
struct PendingPred {
  bool in_use = false;
  char trip[16] = {};
  int32_t predicted_offset_s[4] = {};  // seconds since the aggregator's window_start, per bucket
  uint8_t bucket_has = 0;              // bitmask: bit i set => predicted_offset_s[i] is valid
  uint32_t seq = 0;                    // touch order, for least-recently-used eviction
};

// StatsAggregator accumulates DESIGN §9.2 statistics for one stop_key, over the CSV rows whose
// `ts` falls in [window_start, window_end), across one streaming pass (feedLine() may be called
// once per line of one or more monthly log files, in any order, though chronological order is
// required for the headway-ratio and prediction-matching features to see events in the order
// they actually happened).
//
// Memory: every member is fixed-size; sizeof(StatsAggregator) is asserted < 9216 bytes in
// test_stats/test_main.cpp per DESIGN §9.3. Approximate breakdown (64-bit host build):
//   by_hour_[24] + by_weekday_[7]        : 31 * ~152 B                  ~4.7 KB
//   pending_[kMaxPendingPredTrips]       : 64 * ~44 B                   ~2.8 KB
//   crowd_by_hour_[24] + crowd_by_weekday_[7] : 31 * ~20 B              ~0.6 KB
//   wait_by_hour_[24]                    : 24 * ~16 B                  ~0.4 KB
//   headway_, prediction_[4], scalar counters, stop_key_, window bounds ~0.3 KB
// Intended lifecycle: construct one per /api/stats request, feed it the relevant log lines,
// call toJson() once, then discard it.
class StatsAggregator {
 public:
  StatsAggregator(std::string stop_key, transit::Epoch window_start, transit::Epoch window_end,
                   TzHourWeekdayFn tz_fn = &americaNewYorkLocalHourWeekday);

  // Feeds one CSV line (no trailing newline required; a trailing '\r' is fine). Lines that fail
  // to parse (this includes the header row -- see events.h), that name a different stop_key, or
  // whose `ts` falls outside [window_start, window_end) are silently ignored.
  void feedLine(const char* line, size_t len);

  // Writes the DESIGN §9.3 response shape into `doc`:
  //   stop, days, samples, on_time_pct, mean_late_min,
  //   by_hour[{h,n,mean,p50,p90}] (24 entries, h=0..23, always present even if n==0),
  //   by_weekday[{wd,n,mean,p50,p90}] (7 entries, wd=0(Sun)..6(Sat), always present),
  //   headway{n,bunched,gapped,ratio_hist[20]},
  //   ghost, noshow, outage_min,
  //   prediction[{horizon_s,n,mae_s,bias_s}] (4 entries, in kHorizonBuckets order, always present)
  //   crowding{by_hour[{h,n,mean,dist[6]}] (24), by_weekday[{wd,n,mean,dist[6]}] (7)}
  //   wait_by_hour[{h,n,mean_gap_s,max_gap_s,ghost,noshow}] (24 entries, always present)
  // `doc` is assumed empty/fresh and sized by the caller.
  void toJson(ArduinoJson::JsonDocument& doc) const;

  // --- accessors used by summary.h's summarize() and by tests ---
  uint32_t samples() const { return samples_; }
  double onTimePct() const;    // 0..100; 0 if samples() == 0
  double meanLateMin() const;  // mean over samples with known late_min; 0 if none
  // Hour (0-23) with the highest mean late_min among hours with at least one sample; -1 if none.
  int findWorstHour() const;
  uint32_t ghostCount() const { return ghost_; }

 private:
  std::string stop_key_;
  transit::Epoch window_start_;
  transit::Epoch window_end_;
  TzHourWeekdayFn tz_fn_;

  LatenessBucket by_hour_[24];
  LatenessBucket by_weekday_[7];

  uint32_t samples_ = 0;  // total `arrive` rows in window for this stop (known + unknown late)
  uint32_t on_time_count_ = 0;
  uint32_t late_known_count_ = 0;
  int32_t sum_late_known_ = 0;

  HeadwayStats headway_;
  bool has_prev_scheduled_ = false;
  transit::Epoch prev_scheduled_ts_ = 0;

  uint32_t ghost_ = 0;
  uint32_t noshow_ = 0;

  bool outage_pending_ = false;
  transit::Epoch outage_pending_start_ = 0;
  int64_t outage_seconds_ = 0;

  PendingPred pending_[kMaxPendingPredTrips];
  uint32_t pending_seq_ = 0;
  PredictionBucket prediction_[4];  // parallel to kHorizonBuckets

  CrowdingBucket crowd_by_hour_[24];
  CrowdingBucket crowd_by_weekday_[7];
  WaitBucket wait_by_hour_[24];

  void handlePred(const LogEvent& ev);
  void handleArrive(const LogEvent& ev);
  void handleGhost(const LogEvent& ev);
  void handleNoShow(const LogEvent& ev);
  void handleOutage(const LogEvent& ev);

  PendingPred* findPending(const std::string& trip);
  PendingPred& allocPending(const std::string& trip);
};

}  // namespace transit_stats
