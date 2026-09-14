// OverviewAggregator: streams the SD-card CSV log ONCE for ALL configured stops and Indego
// stations, producing the DESIGN.md §9.3 `GET /api/stats/overview?days=N` JSON shape. Unlike
// StatsAggregator (one stop_key, richer per-stop stats), this is a wide-but-shallow summary
// meant for a landing/overview page: a handful of numbers per stop, plus per-hour bike-share
// availability. Arduino-independent except for ArduinoJson.
#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "transit_core/model.h"
#include "transit_stats/aggregate.h"  // TzHourWeekdayFn, americaNewYorkLocalHourWeekday
#include "transit_stats/events.h"

namespace transit_stats {

// Config's own caps (DESIGN.md §6: "Maximum 8 stops", "bike (max 3 stations)"): a device can
// never legitimately produce more distinct stop_keys or Indego station keys than this in its own
// log, so these are hard caps, not just a memory nicety. Rows for any further, distinct key are
// silently ignored (first-seen key wins the slot; DESIGN §9.3).
constexpr size_t kMaxOverviewStops = 8;
constexpr size_t kMaxOverviewBikeStations = 3;

// Per-hour bike-share accumulator (DESIGN §9.3 "bikes[].by_hour"). n/means come from `bike` rows
// for that station falling in that local hour.
struct BikeHourBucket {
  uint16_t n = 0;
  uint32_t sum_bikes = 0;
  uint32_t sum_ebikes = 0;
  uint32_t sum_docks = 0;
};

// One tracked stop's overview counters (DESIGN §9.3 "stops[]").
struct OverviewStopStats {
  bool in_use = false;
  std::string stop_key;
  uint32_t samples = 0;            // `arrive` rows
  uint32_t on_time_count = 0;      // of samples, SEPTA on-time (see aggregate.h)
  uint32_t late_known_count = 0;
  int64_t sum_late_known = 0;
  uint32_t ghost = 0;
  uint32_t noshow = 0;
  transit::Epoch last_seen_ts = 0;  // max ts of any row (pred/arrive/ghost/noshow/outage) seen
};

// One tracked Indego station's overview counters (DESIGN §9.3 "bikes[]").
struct OverviewBikeStats {
  bool in_use = false;
  std::string station_key;  // e.g. "indego-3468"
  std::string name;         // the note of the last `bike` row seen for this station
  BikeHourBucket by_hour[24];
};

// Memory: every member is fixed-size; aim is < 2 KB (DESIGN §9.3), asserted generously (< 4096
// bytes, to leave headroom without pretending to a false precision) in test_stats/test_main.cpp.
// Approximate breakdown (64-bit host build):
//   stops_[kMaxOverviewStops]             : 8  * ~80 B  ~0.6 KB
//   bikes_[kMaxOverviewBikeStations]      : 3  * ~450 B ~1.3 KB (dominated by by_hour_[24])
//   window bounds, tz_fn_                 :               ~24 B
// Intended lifecycle: construct one per /api/stats/overview request, feed it every log line in
// the requested window across every monthly file, call toJson() once, then discard it.
class OverviewAggregator {
 public:
  // `days` (the JSON response's top-level "days") is derived from window_end - window_start,
  // exactly as StatsAggregator::toJson() derives it -- no separate field to keep in sync.
  OverviewAggregator(transit::Epoch window_start, transit::Epoch window_end,
                      TzHourWeekdayFn tz_fn = &americaNewYorkLocalHourWeekday);

  // Feeds one CSV line. Lines that fail to parse (including the header row), or whose `ts` falls
  // outside [window_start, window_end), are silently ignored. Every other line is attributed to
  // its stop_key (Pred/Arrive/Ghost/NoShow/Outage rows) or Indego station (Bike rows), subject to
  // the kMaxOverviewStops / kMaxOverviewBikeStations caps above.
  void feedLine(const char* line, size_t len);

  // Writes the DESIGN §9.3 overview response shape into `doc`:
  //   days,
  //   stops[{stop,samples,on_time_pct,mean_late_min,ghost,noshow,last_seen_ts}] (one entry per
  //     distinct stop_key seen, in first-seen order),
  //   bikes[{station,name,by_hour[{h,n,bikes,ebikes,docks}] (24 entries, always present)}] (one
  //     entry per distinct Indego station seen, in first-seen order)
  // `doc` is assumed empty/fresh and sized by the caller.
  void toJson(ArduinoJson::JsonDocument& doc) const;

 private:
  transit::Epoch window_start_;
  transit::Epoch window_end_;
  TzHourWeekdayFn tz_fn_;

  OverviewStopStats stops_[kMaxOverviewStops];
  OverviewBikeStats bikes_[kMaxOverviewBikeStations];

  OverviewStopStats* findOrCreateStop(const std::string& key);
  OverviewBikeStats* findOrCreateBike(const std::string& key);

  void handleStopRow(const LogEvent& ev);
  void handleBike(const LogEvent& ev);
};

}  // namespace transit_stats
