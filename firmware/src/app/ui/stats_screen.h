// Stats page: per-stop last-30-days summary (DESIGN.md SS8: "on-time %, mean late, worst hour,
// ghost count, sample count"), sourced from net_poller::getStopSummary() (transit_stats::
// StopSummary, itself derived from a StatsAggregator streamed over the SD log - see
// net_poller.cpp).
#pragma once
#include <lvgl.h>

#include "../config_store.h"

namespace transit_app::ui {

// Builds one row per configured stop (plus a title). Call refreshStatsScreen afterwards (and
// periodically while visible) to populate/update it.
lv_obj_t *createStatsScreen(const Config &cfg);

// Updates every row from the current getStopSummary() cache. A stop with no samples yet (never
// polled, or logging has been off) shows "no data yet" instead of fabricated zeros.
void refreshStatsScreen(lv_obj_t *screen);

}  // namespace transit_app::ui
