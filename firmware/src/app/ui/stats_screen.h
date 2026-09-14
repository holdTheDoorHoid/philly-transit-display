// Stats placeholder page. DESIGN.md SS8/SS9.3 describes per-stop on-time %,
// mean late, worst hour, ghost count, and sample count - all computed by
// transit_stats::StatsAggregator, which doesn't exist yet. This screen just
// holds the page's place in the tap-to-cycle rotation.
#pragma once
#include <lvgl.h>

namespace transit_app::ui {

lv_obj_t *createStatsScreen();

}  // namespace transit_app::ui
