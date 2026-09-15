// Night / service-ended page (DESIGN.md SS6 "night", SS8): shown instead of the arrivals page
// while no configured stop has anything due within device.night.after_min. Big clock, date,
// weather, and each stop's next departure.
#pragma once
#include <lvgl.h>

#include <string>
#include <vector>

#include "../config_store.h"
#include "transit_core/model.h"

namespace transit_app::ui {

lv_obj_t *createNightScreen(const Config &cfg);
void refreshNightScreen(lv_obj_t *screen, const Config &cfg, const transit::Snapshot &snap);

// True when the night page should be up: every stop the main page shows (profiles.h) has
// TRUSTWORTHY data and none of them has an arrival within after_min. A stop with no data at all,
// or whose health is Stale or Unavailable (F13), keeps the arrivals page - its reason text is the
// more useful thing to show, and an empty stop we cannot see is not the same as a quiet one.
// Health::ScheduleOnly does not block the page: schedule rows are real answers (a subway stop
// only ever has those), just not live ones.
bool nightConditionMet(const Config &cfg, const transit::Snapshot &snap, const std::vector<std::string> &shown_keys,
                       transit::Epoch now);

}  // namespace transit_app::ui
