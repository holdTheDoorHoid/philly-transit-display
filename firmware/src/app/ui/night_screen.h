// Night / service-ended page (DESIGN.md SS6 "night", SS8): shown instead of the arrivals page
// while no configured stop has anything due within device.night.after_min. Big clock, date,
// weather, and each stop's next departure.
#pragma once
#include <lvgl.h>

#include "../config_store.h"
#include "transit_core/model.h"

namespace transit_app::ui {

lv_obj_t *createNightScreen(const Config &cfg);
void refreshNightScreen(lv_obj_t *screen, const Config &cfg, const transit::Snapshot &snap);

// True when the night page should be up: every stop the main page shows (profiles.h) has data
// and none of them has an arrival within after_min. A stop with no data at all keeps the
// arrivals page (its "no data"/"poll failed" reason is the more useful thing to show).
bool nightConditionMet(const Config &cfg, const transit::Snapshot &snap, transit::Epoch now);

}  // namespace transit_app::ui
