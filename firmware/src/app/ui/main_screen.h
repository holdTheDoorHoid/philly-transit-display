// Main screen: header + one panel per configured stop + alert ticker.
// DESIGN.md SS8. Renders from a transit::Snapshot (demo_data.h supplies one
// until transit_core produces real ones) at the panel/row structure defined
// by `cfg.stops` - see main_screen.cpp's top-of-file comment for why the
// two are separate inputs.
#pragma once
#include <lvgl.h>

#include <string>
#include <vector>

#include "../config_store.h"
#include "transit_core/model.h"

namespace transit_app::ui {

// Builds the screen and all its child widgets once. Call refreshMainScreen
// afterwards (and periodically) to populate/update it.
lv_obj_t *createMainScreen(const Config &cfg);

// Updates the clock, Wi-Fi indicator, "updated Ns ago" header text, and
// every stop panel's rows from `snap`. Panels are matched to `cfg.stops` by
// StopConfig::key <-> StopSnapshot::key; a configured stop with no matching
// entry in `snap` shows "no data" instead of stale/wrong numbers.
// Test hook (GET /api/debug/ui): which alternative panels are currently hidden, and the ticker text.
// rows_debug: one line per visible arrival row, "key|dest|minutes|icons h=0 len=27 w=52|word h=1 'few seats'".
void mainScreenDebug(lv_obj_t *screen, std::vector<std::string> &hidden_panels, std::string &ticker_text,
                     std::string &rows_debug);

void refreshMainScreen(lv_obj_t *screen, const Config &cfg, const transit::Snapshot &snap);

}  // namespace transit_app::ui
