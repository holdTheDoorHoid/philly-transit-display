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
// ui.cpp calls this before the first page build and again from rebuildScreens().
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
// 16 px icon font (src/fonts/lv_font_icons_16.c): digits and space from Montserrat plus FontAwesome
// chair, person, bicycle, bolt and parking glyphs; crowding meter and the Indego section.
const lv_font_t *fontIcons();
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
// One Indego station's counts for a recolor-enabled label. icons=true: "[bicycle] 5  [bolt] 2
// [P] 7" for fontIcons(); false: "5 bikes, 2 e-bikes, 7 docks" for fontSmall(). Each count is
// red at 0 and amber at 1-2 (DESIGN.md SS8), the low-station cue the owner asked for.
std::string bikeCounts(int classic, int ebikes, int docks, bool icons);

// "11:42p" for the night clock and the "next bus" lines.
std::string clockLabel(transit::Epoch when);

// model.h's StopConfig has no per-route color field (DESIGN.md SS8 says
// "route colour from config or default" but the schema/struct never grew
// one) - this is the "default" every route badge uses for now.
lv_color_t routeBadgeColor();

// ---- Shared building blocks (DESIGN.md SS8: the pages share one visual language) ----
//
// Every page is: a header strip, then panels in a padded column. The main page defined the look
// first; the stats and device pages are built from the same pieces so they match by construction
// rather than by copying numbers around.

// Plain container: paints its background (LVGL's default bg_opa is 0 with no theme compiled in,
// so a bg_color alone paints nothing - the first builds learned this), never scrolls, and is NOT
// clickable, so a tap on it reaches the screen's tap-to-cycle handler instead of stopping at the
// child. Square corners are the default: LV_DRAW_SW_COMPLEX is 0 (lv_conf.h, flash budget) and
// LVGL then skips a rounded rectangle entirely rather than drawing it square, so nothing here
// ever sets a radius.
lv_obj_t *makeBox(lv_obj_t *parent);
lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);

// The header strip every page starts with: 10 % of the height (never under 20 px), panel colour,
// a flex row with its items spread from edge to edge and centred vertically.
int32_t headerHeight(int32_t h);
lv_obj_t *makeHeader(lv_obj_t *screen, int32_t h);
// The area under the header: transparent, 4 px padding and row gap, a flex column that takes the
// rest of the screen.
lv_obj_t *makePanelsArea(lv_obj_t *screen);
// A stop-style panel inside it: panel colour, 6 px padding, flex column, and flex_grow so sibling
// panels share the height equally (the main page's stop panels).
lv_obj_t *makePanel(lv_obj_t *parent);
// A transparent flex row of `gap`-spaced, vertically centred items, as wide as its parent.
lv_obj_t *makeRow(lv_obj_t *parent, int32_t gap);
// White text on routeBadgeColor() with 4 px side padding - the route badge on every arrival row.
lv_obj_t *makeRouteBadge(lv_obj_t *parent, const lv_font_t *font, const std::string &route);

// The 24-bit value of a colour for LVGL's inline recolor command in a label with
// lv_label_set_recolor(true): snprintf(buf, n, "#%06x %s#", colorHex(c), text). Commands must
// not span a line break; close one before a '\n' and open another after it. The pages compose
// their text this way rather than with std::string concatenation, which cost ~1 KB of flash per
// page in string template instantiations (measured 2026-09-16).
inline uint32_t colorHex(lv_color_t c) {
  return lv_color_to_u32(c) & 0xFFFFFFu;
}

// ---- Wi-Fi signal, drawn like a phone's status bar ----
//
// Four bars of increasing height instead of the old "wifi 3" glyph-plus-digit: RSSI >= -55 dBm
// lights 4, >= -65 3, >= -75 2, >= -85 1, weaker or not connected 0. No glyph, no number.
int wifiBarCount(int rssi, bool connected);
// Builds the widget at the height of one line of the small font (a phone status-bar icon sits at
// the height of the text beside it), shrunk only inside a header shorter than that. All bars
// start dim.
lv_obj_t *makeWifiBars(lv_obj_t *parent, int32_t header_h);
// Lights `count` bars solid in `on`; the others stay visible as a 1:3 blend of `on` into `bg`
// (lv_color_mix(on, bg, 64)), so "one bar" and "no signal" both still read as a signal icon.
// Pass the header's stale colours while it is amber so the icon recolours with the rest.
void setWifiBars(lv_obj_t *bars, int count, lv_color_t on, lv_color_t bg);

}  // namespace transit_app::ui
