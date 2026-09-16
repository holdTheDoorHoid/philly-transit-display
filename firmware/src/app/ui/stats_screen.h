// Stats page: per-stop last-30-days summary (DESIGN.md SS8: "on-time %, mean late, worst hour,
// ghost count, sample count"), sourced from net_poller::getStopSummary() (transit_stats::
// StopSummary, itself derived from a StatsAggregator streamed over the SD log - see
// net_poller.cpp).
#pragma once
#include <lvgl.h>

#include "../config_store.h"

namespace transit_app::ui {

// Builds the header strip and one stop-style panel per configured stop, each with the on-time
// percentage over an on-time meter, stat tiles (mean lateness, worst hour, ghosts) and a
// sample/inferred footer, in whichever of three layouts the panel height allows
// (stats_screen.cpp Layout). Built when the page is
// tapped to and deleted when tapped away from (ui.cpp). Call refreshStatsScreen afterwards (and
// periodically while visible) to populate/update it.
lv_obj_t *createStatsScreen(const Config &cfg);

// Updates every panel from the current getStopSummary() cache. Never touches the SD card itself
// (F27): the scan happens on the poller task, so a stop whose summary has not been computed yet
// shows "loading...", and one with no samples shows "no data yet" - neither ever shows fabricated
// zeros. On-time % renders as a dash when no arrival's lateness was ever known (DESIGN.md SS9.2),
// and each panel carries its sample count and how many of those arrivals were inferred.
void refreshStatsScreen(lv_obj_t *screen);

}  // namespace transit_app::ui
