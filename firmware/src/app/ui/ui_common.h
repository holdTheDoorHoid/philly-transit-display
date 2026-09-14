// Shared LVGL helpers for the three screens: palette, badge formatting, and
// runtime-resolution-driven sizing (DESIGN.md SS8: "The UI must lay out
// from the runtime display resolution, not hardcoded 480x320").
#pragma once
#include <lvgl.h>

#include <string>

#include "transit_core/model.h"

namespace transit_app::ui {

lv_color_t colorBg();
lv_color_t colorPanelBg();
lv_color_t colorText();
lv_color_t colorSubtext();
lv_color_t colorOnTime();   // green
lv_color_t colorLate();     // red
lv_color_t colorEarly();    // blue
lv_color_t colorScheduled();  // grey
lv_color_t colorSkipped();  // orange
lv_color_t colorStale();    // amber, for the header when data is stale

// "12", "Due" (< 1 min out), "Now" (<= 0) - DESIGN.md SS8.
std::string minutesLabel(transit::Epoch eta_s);

struct Badge {
  std::string text;
  lv_color_t color;
};

// DESIGN.md SS8: green "on time" (-1..+5 min, SEPTA's on-time window),
// red "+13", blue "-2", grey "sched", orange "skip".
Badge badgeFor(const transit::Arrival &a);

// Current landscape resolution of the running display.
void screenSize(int32_t &w, int32_t &h);

// DESIGN.md SS8: "320x240 gets 2 rows per stop and smaller fonts; 480x320
// gets 3 rows."
int rowsPerStop(int32_t h);
const lv_font_t *fontBig(int32_t h);    // the big "minutes" number
const lv_font_t *fontBody(int32_t h);   // destination / title text
const lv_font_t *fontSmall(int32_t h);  // badges, footer, secondary text

// model.h's StopConfig has no per-route color field (DESIGN.md SS8 says
// "route colour from config or default" but the schema/struct never grew
// one) - this is the "default" every route badge uses for now.
lv_color_t routeBadgeColor();

}  // namespace transit_app::ui
