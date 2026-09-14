// StopSummary: the tiny derived view of a StatsAggregator used by the on-device stats page
// (DESIGN.md §8: "Stats page: per stop, last 30 days: on-time %, mean late, worst hour, ghost
// count, sample count"). Arduino-independent.
#pragma once

#include <cstdint>

namespace transit_stats {

class StatsAggregator;

// sizeof(StopSummary) is 16 bytes: cheap enough to keep one per configured stop (up to 8) in
// LVGL UI state with no meaningful memory cost.
struct StopSummary {
  uint32_t samples = 0;       // total `arrive` events in the aggregator's window
  float on_time_pct = 0.0f;   // 0..100, SEPTA on-time definition (see aggregate.h)
  float mean_late_min = 0.0f; // mean over samples with known late_min
  int8_t worst_hour = -1;     // hour (0-23) with the highest mean lateness; -1 if no data
  uint32_t ghosts = 0;        // ghost count in the window
};

// Derives a StopSummary from an already-fed StatsAggregator (call after feedLine()/before or
// after toJson(); summarize() does not mutate the aggregator).
StopSummary summarize(const StatsAggregator& agg);

}  // namespace transit_stats
