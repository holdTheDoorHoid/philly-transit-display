// Shared LVGL helpers for the three screens: palette, badge formatting, and
// runtime-resolution-driven sizing (DESIGN.md SS8: "The UI must lay out
// from the runtime display resolution, not hardcoded 480x320").
#pragma once
#include <lvgl.h>

#include <string>

#include "transit_core/model.h"

namespace transit_app::ui {

// Selects the palette every colour*() below returns: "dark", or anything else for the light
// default (config.device.theme, DESIGN.md SS6). Screens pick colours up when they are built, so
// ui.cpp calls this before buildScreens()/rebuildScreens().
void setTheme(const std::string &name);
bool isDarkTheme();

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
lv_color_t colorOnStale();  // text colour to use on that amber

// "12", "Due" (< 1 min out), "Now" (<= 0) - DESIGN.md SS8.
std::string minutesLabel(transit::Epoch eta_s);

// minutesLabel(), except that an arrival an hour or more away shows as its local clock time
// ("1:14a") instead of a minute count: a scheduled trip hours out (overnight service, or SEPTA
// answering with the wrong service day - septa_source.h) reads as a time, not as "958".
// `when` is the arrival's effective() epoch.
std::string etaLabel(transit::Epoch eta_s, transit::Epoch when);

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
// gets 3 rows." Large-text mode (device.large_text) is always 2.
int rowsPerStop(int32_t h, bool large_text = false);
const lv_font_t *fontBig(int32_t h);    // the big "minutes" number
const lv_font_t *fontHuge();            // 48 px digits-only subset (src/fonts/): large text, night clock
const lv_font_t *fontIcons();           // 16 px chair + person glyphs (src/fonts/): crowding meter
const lv_font_t *fontBody(int32_t h);   // destination / title text
const lv_font_t *fontSmall(int32_t h);  // badges, footer, secondary text

// DESIGN.md SS6 stops[].title_style: "17 Southbound → 20th-Johnston" (label_dest, default),
// the label alone, "17 → 20th-Johnston • 19th St & Mifflin St", or title_text verbatim.
std::string panelTitle(const transit::StopConfig &s);

// SEPTA estimated_seat_availability as a word: open, few seats, standing, packed, full, empty;
// empty string when SEPTA has no estimate (NOT_AVAILABLE / blank).
std::string crowdingText(const std::string &seats);
// The same estimate as a three-slot icon meter for a label drawn with fontIcons() and
// lv_label_set_recolor(true). style "seats": chairs fill while you can sit (3/2/1, green then
// amber), people fill once you stand (1/2/3, amber then red). style "crowd": people only,
// 1 green / 2 amber / 3 red. Unused slots are drawn dim so the meter keeps its width.
std::string crowdingIcons(const std::string &seats, const std::string &style);

// "11:42p" for the night clock and the "next bus" lines.
std::string clockLabel(transit::Epoch when);

// model.h's StopConfig has no per-route color field (DESIGN.md SS8 says
// "route colour from config or default" but the schema/struct never grew
// one) - this is the "default" every route badge uses for now.
lv_color_t routeBadgeColor();

}  // namespace transit_app::ui
