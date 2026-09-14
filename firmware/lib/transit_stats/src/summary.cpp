#include "transit_stats/summary.h"

#include "transit_stats/aggregate.h"

namespace transit_stats {

StopSummary summarize(const StatsAggregator& agg) {
  StopSummary s;
  s.samples = agg.samples();
  s.on_time_pct = static_cast<float>(agg.onTimePct());
  s.mean_late_min = static_cast<float>(agg.meanLateMin());
  s.worst_hour = static_cast<int8_t>(agg.findWorstHour());
  s.ghosts = agg.ghostCount();
  return s;
}

}  // namespace transit_stats
