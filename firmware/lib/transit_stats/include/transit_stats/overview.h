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
#include <vector>

#include "transit_core/model.h"
#include "transit_stats/aggregate.h"  // TzHourWeekdayFn, americaNewYorkLocalHourWeekday
#include "transit_stats/events.h"

namespace transit_stats {

// Config's own caps (DESIGN.md §6: "Maximum 8 stops", "bike (max 3 stations)"). A device can
// never watch more stops than this AT ONE TIME -- but a 30-day log can easily contain more
// distinct stop_keys than that, because the user is allowed to change which stops they watch. The
// old first-seen-wins rule then handed all 8 slots to the stops they USED to watch and silently
// dropped the ones on the screen right now (F28), which is exactly backwards: the overview page
// exists to describe the current configuration.
//
// So the caller passes the keys it currently cares about (see the constructor) and those get
// reserved slots; leftover slots go to other keys in first-seen order; anything past that is
// counted in excluded_stops/excluded_bikes and reported in the JSON, never silently dropped.
constexpr size_t kMaxOverviewStops = 8;
constexpr size_t kMaxOverviewBikeStations = 3;

// How many DISTINCT excluded keys can be counted exactly. Beyond this the count is a lower bound
// (the JSON says "at least"); with config capped at 8 stops a window would need more than 24
// different historical stop_keys to reach it.
constexpr size_t kMaxExcludedKeysTracked = 24;

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
  bool reserved = false;  // named by the caller as currently configured; keeps its slot even at 0 rows
  std::string stop_key;
  uint32_t samples = 0;            // `arrive` rows
  uint32_t on_time_count = 0;      // of samples with a KNOWN late_min, SEPTA on-time (aggregate.h)
  uint32_t late_known_count = 0;   // denominator of on_time_pct (F21)
  int64_t sum_late_known = 0;
  uint32_t inferred = 0;           // `arrive` rows carrying an inference marker (events.h)
  uint32_t ghost = 0;
  uint32_t noshow = 0;
  transit::Epoch last_seen_ts = 0;  // max ts of any row (pred/arrive/ghost/noshow/outage) seen
  // Per-stop outage pairing, same rules as StatsAggregator (F23): intervals are clipped to the
  // window, an interval with no `end` row runs to window_end.
  bool outage_open = false;
  transit::Epoch outage_open_start = 0;
  int64_t outage_seconds = 0;
};

// One tracked Indego station's overview counters (DESIGN §9.3 "bikes[]").
struct OverviewBikeStats {
  bool in_use = false;
  bool reserved = false;    // see OverviewStopStats::reserved
  std::string station_key;  // e.g. "indego-3468"
  std::string name;         // the note of the last `bike` row seen for this station
  BikeHourBucket by_hour[24];
};

// Memory: every member is fixed-size; aim is < 2 KB (DESIGN §9.3), asserted generously (< 4096
// bytes, to leave headroom without pretending to a false precision) in test_stats/test_main.cpp.
// Approximate breakdown (64-bit host build):
//   stops_[kMaxOverviewStops]             : 8  * ~120 B ~1.0 KB
//   bikes_[kMaxOverviewBikeStations]      : 3  * ~450 B ~1.3 KB (dominated by by_hour_[24])
//   excluded-key hash tables              : 2  * 24 * 4 B ~0.2 KB
//   window bounds, tz_fn_                 :               ~24 B
// Intended lifecycle: construct one per /api/stats/overview request, feed it every log line in
// the requested window across every monthly file, call toJson() once, then discard it.
class OverviewAggregator {
 public:
  // `days` (the JSON response's top-level "days") is derived from window_end - window_start,
  // exactly as StatsAggregator::toJson() derives it -- no separate field to keep in sync.
  //
  // reserved_stop_keys / reserved_bike_keys: the keys the device is configured for RIGHT NOW
  // (StopConfig::key, and "indego-<id>" for each configured station). Each gets a slot up front,
  // in the order given, and appears in the JSON even with zero rows in the window -- "you just
  // added this stop, no data yet" is a useful answer; "this stop does not exist" is a wrong one.
  // Callers with no configuration to hand (tests, one-off tooling) may pass empty vectors and get
  // the old pure first-seen behaviour.
  OverviewAggregator(transit::Epoch window_start, transit::Epoch window_end,
                      const std::vector<std::string>& reserved_stop_keys,
                      const std::vector<std::string>& reserved_bike_keys,
                      TzHourWeekdayFn tz_fn = &americaNewYorkLocalHourWeekday);

  // Convenience overload for callers with no current configuration: no reserved keys.
  OverviewAggregator(transit::Epoch window_start, transit::Epoch window_end,
                      TzHourWeekdayFn tz_fn = &americaNewYorkLocalHourWeekday);

  // Feeds one CSV line. Lines that fail to parse (including the header row, blank lines and
  // over-long records) are silently ignored, as are rows whose `ts` falls outside
  // [window_start, window_end) -- with the same single exception StatsAggregator makes: `outage`
  // rows are paired regardless of ts and clipped to the window (F23), though an out-of-window
  // outage row never creates a slot for a stop that has nothing else in the window.
  // Every other line is attributed to its stop_key (Pred/Arrive/Ghost/NoShow/Outage rows) or
  // Indego station (Bike rows), subject to the caps and reservations above.
  void feedLine(const char* line, size_t len);

  // Writes the DESIGN §9.3 overview response shape into `doc`:
  //   days, samples (all stops), inferred (all stops),
  //   stops[{stop,samples,late_known,on_time_pct (null when late_known==0),mean_late_min (same),
  //          inferred,ghost,noshow,outage_min,coverage,last_seen_ts}] (reserved keys first, then
  //     other keys in first-seen order),
  //   bikes[{station,name,samples,by_hour[{h,n,bikes,ebikes,docks}] (24 entries, always present)}]
  //   excluded_stops, excluded_bikes (distinct keys in the window that had no slot -- the UI must
  //     disclose them rather than let the page imply it is showing everything)
  // `doc` is assumed empty/fresh and sized by the caller.
  void toJson(ArduinoJson::JsonDocument& doc) const;

 private:
  transit::Epoch window_start_;
  transit::Epoch window_end_;
  TzHourWeekdayFn tz_fn_;

  OverviewStopStats stops_[kMaxOverviewStops];
  OverviewBikeStats bikes_[kMaxOverviewBikeStations];

  // Excluded-key accounting. Keys are kept as 32-bit FNV-1a hashes so a long tail of historical
  // stop_keys cannot grow this structure; a collision can only undercount an exclusion by one.
  uint32_t excluded_stop_hashes_[kMaxExcludedKeysTracked] = {};
  uint32_t excluded_bike_hashes_[kMaxExcludedKeysTracked] = {};
  uint16_t excluded_stops_ = 0;
  uint16_t excluded_bikes_ = 0;

  OverviewStopStats* findStop(const std::string& key);
  OverviewStopStats* findOrCreateStop(const std::string& key);
  OverviewBikeStats* findOrCreateBike(const std::string& key);
  void noteExcluded(const std::string& key, uint32_t (&hashes)[kMaxExcludedKeysTracked],
                     uint16_t& count);
  int64_t overlapWithWindow(transit::Epoch a, transit::Epoch b) const;

  void handleStopRow(const LogEvent& ev);
  void handleOutageRow(const LogEvent& ev);
  void handleBike(const LogEvent& ev);
};

}  // namespace transit_stats
