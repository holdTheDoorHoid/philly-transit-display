// StopSummary: the tiny derived view of a StatsAggregator used by the on-device stats page
// (DESIGN.md §8: "Stats page: per stop, last 30 days: on-time %, mean late, worst hour, ghost
// count, sample count"). Arduino-independent.
#pragma once

#include <cstdint>

namespace transit_stats {

class StatsAggregator;

// sizeof(StopSummary) is 24 bytes: cheap enough to keep one per configured stop (up to 8) in
// LVGL UI state with no meaningful memory cost.
struct StopSummary {
  uint32_t samples = 0;       // total `arrive` events in the aggregator's window
  uint32_t late_known = 0;    // of those, how many carried a known late_min
  // on_time_pct IS ONLY MEANINGFUL WHEN has_on_time IS TRUE. With no known-lateness sample there
  // is no percentage to show, and 0.0f would read on the stats screen as "0% on time" -- the
  // worst possible answer to a question we simply cannot answer (F21). Render a dash instead.
  bool has_on_time = false;
  float on_time_pct = 0.0f;   // 0..100 over late_known samples, SEPTA definition (aggregate.h)
  float mean_late_min = 0.0f; // mean over samples with known late_min; 0 when late_known == 0
  int8_t worst_hour = -1;     // hour (0-23) with the highest mean lateness; -1 if no data
  uint32_t ghosts = 0;        // ghost count in the window
};

// Derives a StopSummary from an already-fed StatsAggregator (call after feedLine()/before or
// after toJson(); summarize() does not mutate the aggregator).
StopSummary summarize(const StatsAggregator& agg);

}  // namespace transit_stats
