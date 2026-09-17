#include "ui_common.h"

#include <cstdio>
#include <ctime>

LV_FONT_DECLARE(lv_font_montserrat_48_digits)
LV_FONT_DECLARE(lv_font_icons_16)

namespace transit_app::ui {

namespace {
bool g_dark = false;
}  // namespace

void setTheme(const std::string &name) {
  g_dark = (name == "dark");
}


// Light palette: every text colour clears WCAG AA (4.5:1) against the white panel background.
// The first build shipped only the dark palette, which on a panel that needs colour inversion
// (docs/hardware.md) came out as pale cyan and pink badges on white. Dark palette: the original,
// with the secondary text lifted a little for contrast.
lv_color_t colorBg() {
  return lv_color_hex(g_dark ? 0x101418 : 0xE9ECF0);
}
lv_color_t colorPanelBg() {
  return lv_color_hex(g_dark ? 0x1C2126 : 0xFFFFFF);
}
lv_color_t colorText() {
  return lv_color_hex(g_dark ? 0xF0F0F0 : 0x14181C);
}
lv_color_t colorSubtext() {
  return lv_color_hex(g_dark ? 0xA9B4BE : 0x4A5560);
}
lv_color_t colorOnTime() {
  return lv_color_hex(g_dark ? 0x3DCB55 : 0x1B7F3B);
}
lv_color_t colorLate() {
  return lv_color_hex(g_dark ? 0xF05654 : 0xC62828);
}
lv_color_t colorEarly() {
  return lv_color_hex(g_dark ? 0x5B9BFF : 0x1D4ED8);
}
lv_color_t colorScheduled() {
  return lv_color_hex(g_dark ? 0x9AA0A8 : 0x5B6673);
}
lv_color_t colorSkipped() {
  return lv_color_hex(g_dark ? 0xF5A030 : 0xB45309);
}
lv_color_t colorStale() {
  return lv_color_hex(0xF5B301);  // amber header in either theme...
}
lv_color_t colorOnStale() {
  return lv_color_hex(0x14181C);  // ...always with dark text on it
}
lv_color_t routeBadgeColor() {
  return lv_color_hex(g_dark ? 0x274B8F : 0x1E4A9C);  // SEPTA-ish blue, white text
}

std::string minutesLabel(transit::Epoch eta_s) {
  if (eta_s <= 0) {
    return "Now";
  }
  if (eta_s < 60) {
    return "Due";
  }
  return std::to_string((eta_s + 30) / 60);
}

std::string etaLabel(transit::Epoch eta_s, transit::Epoch when) {
  if (eta_s >= 60 * 60 && when > 0) {
    time_t t = (time_t)when;
    struct tm local_tm;
    localtime_r(&t, &local_tm);
    int hour12 = local_tm.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d%s", hour12, local_tm.tm_min, local_tm.tm_hour < 12 ? "a" : "p");
    return buf;
  }
  return minutesLabel(eta_s);
}

Badge badgeFor(const transit::Arrival &a) {
  if (a.status == transit::Status::Skipped) {
    return {"skip", colorSkipped()};
  }
  if (!a.late_known) {
    return {"sched", colorScheduled()};
  }
  if (a.late_min >= -1 && a.late_min <= 5) {
    return {"on time", colorOnTime()};
  }
  if (a.late_min > 5) {
    return {"+" + std::to_string(a.late_min), colorLate()};
  }
  return {std::to_string(a.late_min), colorEarly()};  // already has a '-' sign
}

void screenSize(int32_t &w, int32_t &h) {
  lv_display_t *disp = lv_display_get_default();
  w = lv_display_get_horizontal_resolution(disp);
  h = lv_display_get_vertical_resolution(disp);
}

int rowsPerStop(int32_t h, bool large_text) {
  if (large_text) return 2;
  return h >= 320 ? 3 : 2;
}

// Colour for a bike/dock count: red when none, amber when one or two, otherwise plain text.
static uint32_t countHex(int n) {
  if (n <= 0) return g_dark ? 0xF05654 : 0xC62828;  // == colorLate()
  if (n <= 2) return g_dark ? 0xF5A030 : 0xB45309;  // == colorSkipped() (amber)
  return g_dark ? 0xF0F0F0 : 0x14181C;              // == colorText()
}
static void appendColored(std::string &out, uint32_t hex, const std::string &body) {
  char h[12];
  snprintf(h, sizeof h, "#%06x ", (unsigned)hex);
  out += h;
  out += body;
  out += '#';
}

std::string bikeCounts(int classic, int ebikes, int docks, bool icons) {
  static const char kBike[] = "\xEF\x88\x86";  // U+F206 bicycle
  static const char kBolt[] = "\xEF\x83\xA7";  // U+F0E7 bolt (e-bikes)
  static const char kDock[] = "\xEF\x95\x80";  // U+F540 parking (free docks)
  std::string out;
  if (icons) {
    appendColored(out, countHex(classic), std::string(kBike) + " " + std::to_string(classic));
    out += "  ";
    appendColored(out, countHex(ebikes), std::string(kBolt) + " " + std::to_string(ebikes));
    out += "  ";
    appendColored(out, countHex(docks), std::string(kDock) + " " + std::to_string(docks));
  } else {
    appendColored(out, countHex(classic), std::to_string(classic));
    out += " bikes, ";
    appendColored(out, countHex(ebikes), std::to_string(ebikes));
    out += " e-bikes, ";
    appendColored(out, countHex(docks), std::to_string(docks));
    out += " docks";
  }
  return out;
}

const lv_font_t *fontIcons() {
  return &lv_font_icons_16;
}

const lv_font_t *fontHuge() {
  return &lv_font_montserrat_48_digits;
}

std::string clockLabel(transit::Epoch when) {
  if (when <= 0) return "--:--";
  time_t t = (time_t)when;
  struct tm lt;
  localtime_r(&t, &lt);
  int hour12 = lt.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d%s", hour12, lt.tm_min, lt.tm_hour < 12 ? "a" : "p");
  return buf;
}

std::string panelTitle(const transit::StopConfig &s) {
  const char *bullet = "\xE2\x80\xA2";  // U+2022: the built-in font lacks the middle dot (U+00B7)
  std::string label = s.label.empty() ? s.route : s.label;
  std::string title;
  if (s.title_style == "custom") {
    title = s.title_text.empty() ? label : s.title_text;
  } else if (s.title_style == "label") {
    title = label;
  } else if (s.title_style == "route_dest_stop") {
    if (s.mode == transit::Mode::Rail) {
      title = s.station + (s.direction.empty() ? "" : " (" + s.direction + ")");
    } else {
      title = s.route + " " LV_SYMBOL_RIGHT " " + s.headsign + " " + bullet + " " + s.stop_name;
    }
  } else {  // label_dest
    if (s.mode == transit::Mode::Rail) {
      const char *dir = s.direction == "N" ? "Northbound" : s.direction == "S" ? "Southbound" : "";
      title = label + (dir[0] ? std::string(" (") + dir + ")" : "");
    } else {
      title = label + (s.headsign.empty() ? "" : " " LV_SYMBOL_RIGHT " " + s.headsign);
    }
  }
  // DESIGN.md SS4.6/SS8: subway is schedule-only in v1; say so rather than showing "sched" on
  // every row and letting the user wonder.
  if (s.mode == transit::Mode::Subway) title += std::string(" ") + bullet + " schedule only";
  return title;
}

// SEPTA's six crowding levels in order, 0 = emptiest. -1 when there is no estimate.
static int crowdingLevel(const std::string &seats) {
  if (seats == "EMPTY") return 0;
  if (seats == "MANY_SEATS_AVAILABLE") return 1;
  if (seats == "FEW_SEATS_AVAILABLE") return 2;
  if (seats == "STANDING_ROOM_ONLY") return 3;
  if (seats == "CRUSHED_STANDING_ROOM_ONLY") return 4;
  if (seats == "FULL") return 5;
  return -1;
}

std::string crowdingText(const std::string &seats) {
  // "seats" alone for MANY_SEATS_AVAILABLE read as a truncated label on the panel, so the
  // scale is now open / few seats / standing / packed / full (plus SEPTA's rare "empty").
  static const char *const kWords[] = {"empty", "open", "few seats", "standing", "packed", "full"};
  int level = crowdingLevel(seats);
  return level < 0 ? std::string() : kWords[level];
}

std::string crowdingIcons(const std::string &seats, const std::string &style) {
  int level = crowdingLevel(seats);
  if (level < 0) return "";
  static const char kChair[] = "\xEF\x9B\x80";   // U+F6C0 chair
  static const char kPerson[] = "\xEF\x80\x87";  // U+F007 person silhouette (the U+F183 stick figure was 6 px wide)
  const char *glyph = kPerson;
  int lit = 3;
  uint32_t color;
  const uint32_t green = g_dark ? 0x3DCB55 : 0x1B7F3B;   // == colorOnTime()
  const uint32_t amber = g_dark ? 0xF5A030 : 0xB45309;   // == colorSkipped()
  const uint32_t red = g_dark ? 0xF05654 : 0xC62828;     // == colorLate()
  const uint32_t dim = g_dark ? 0x3E464E : 0xC9D0D6;     // unused slots
  if (style == "crowd") {
    lit = level <= 1 ? 1 : (level <= 3 ? 2 : 3);
    color = lit == 1 ? green : (lit == 2 ? amber : red);
  } else {
    switch (level) {
      case 0: glyph = kChair; lit = 3; color = green; break;
      case 1: glyph = kChair; lit = 2; color = green; break;
      case 2: glyph = kChair; lit = 1; color = amber; break;
      case 3: lit = 1; color = amber; break;
      case 4: lit = 2; color = red; break;
      default: lit = 3; color = red; break;
    }
  }
  // LVGL recolor syntax: '#' + six hex digits + ' ' + text + '#'; commands may follow each other.
  // The buffer must hold all 8 characters plus the NUL: with char hex[8] snprintf dropped the
  // trailing space, LVGL never saw the end of the colour parameter, and the meter laid out at
  // zero width (found with /api/debug/ui rows on 2026-09-14).
  char hex[12];
  std::string out;
  snprintf(hex, sizeof hex, "#%06x ", (unsigned)color);
  out += hex;
  for (int i = 0; i < lit; ++i) out += glyph;
  out += '#';
  if (lit < 3) {
    snprintf(hex, sizeof hex, "#%06x ", (unsigned)dim);
    out += hex;
    for (int i = lit; i < 3; ++i) out += glyph;
    out += '#';
  }
  return out;
}

#ifdef UI_SIM
// firmware/sim only: both fonts are linked on the host, and the simulator flips this per render
// to show the 240-tall boards' layout with their font. Never defined in a board build.
bool g_sim_small_board = false;
#endif

const lv_font_t *fontBig(int32_t /*h*/) {
  // Compile-time per board, not per rotation: the 320x240 boards (2.4"/2.8", which define
  // LV_FONT_MONTSERRAT_20) use 20 px in both orientations. Choosing 28 for portrait at runtime
  // kept both fonts linked and pushed the 2.4" capacitive build 2 KB past its 1.9 MB slot;
  // Montserrat 28 alone is ~32 KB of flash (firmware/README.md).
#if defined(UI_SIM)
  return g_sim_small_board ? &lv_font_montserrat_20 : &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_20
  return &lv_font_montserrat_20;
#else
  return &lv_font_montserrat_28;
#endif
}
const lv_font_t *fontBody(int32_t h) {
  // Flash diet: only Montserrat 14/20/28 are enabled (lv_conf.h) - this used to be 16 on the
  // taller panels, but that was the only caller of size 16 in the whole UI, so sharing 14 here
  // drops a whole embedded bitmap font from flash for one row of body text being 2px smaller.
  (void)h;
  return &lv_font_montserrat_14;
}
const lv_font_t *fontSmall(int32_t /*h*/) {
  return &lv_font_montserrat_14;
}

// ---- Shared building blocks ----

lv_obj_t *makeBox(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  // border_width and radius are already 0 with no theme compiled in; setting them again would
  // only cost a style slot per box (LVGL's 36 KB pool, lv_conf.h).
  lv_obj_set_scrollable(o, false);
  lv_obj_set_clickable(o, false);
  return o;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

int32_t headerHeight(int32_t h) {
  return h / 10 > 20 ? h / 10 : 20;
}

lv_obj_t *makeHeader(lv_obj_t *screen, int32_t h) {
  lv_obj_t *header = makeBox(screen);
  lv_obj_set_size(header, lv_pct(100), headerHeight(h));
  lv_obj_set_style_bg_color(header, colorPanelBg(), 0);
  lv_obj_set_style_pad_hor(header, 8, 0);
  lv_obj_set_style_pad_ver(header, 2, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  return header;
}

lv_obj_t *makePanelsArea(lv_obj_t *screen) {
  lv_obj_t *area = makeBox(screen);
  lv_obj_set_size(area, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_grow(area, 1);
  lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(area, 4, 0);
  lv_obj_set_style_pad_row(area, 4, 0);
  lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
  return area;
}

lv_obj_t *makePanel(lv_obj_t *parent) {
  lv_obj_t *panel = makeBox(parent);
  lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_grow(panel, 1);
  lv_obj_set_style_bg_color(panel, colorPanelBg(), 0);
  lv_obj_set_style_pad_all(panel, 6, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  return panel;
}

lv_obj_t *makeRow(lv_obj_t *parent, int32_t gap) {
  lv_obj_t *row = makeBox(parent);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_column(row, gap, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  return row;
}

lv_obj_t *makeRouteBadge(lv_obj_t *parent, const lv_font_t *font, const std::string &route) {
  lv_obj_t *badge = makeLabel(parent, font, lv_color_white());
  lv_obj_set_style_bg_color(badge, routeBadgeColor(), 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(badge, 4, 0);
  lv_label_set_text(badge, route.c_str());
  return badge;
}

// ---- Wi-Fi bars ----

int wifiBarCount(int rssi, bool connected) {
  if (!connected) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

lv_obj_t *makeWifiBars(lv_obj_t *parent, int32_t header_h) {
  // Sized like a phone's status-bar icon: the tallest bar is one line of the small font (16 px),
  // so the icon sits next to "updated 12 s ago" at the height of its text instead of towering
  // over it, and it is the same size on every board. Only a header shorter than that (the 20 px
  // floor) shrinks it. Bars are 4 px with 2 px gaps at that height, 3 px and 1 px when shrunk.
  int32_t col_h = lv_font_get_line_height(fontSmall(0));
  if (col_h > header_h - 4) col_h = header_h - 4;
  int32_t bar_w = col_h >= 14 ? 4 : 3;
  int32_t gap = bar_w / 2;

  lv_obj_t *bars = makeBox(parent);
  lv_obj_set_style_bg_opa(bars, LV_OPA_TRANSP, 0);
  lv_obj_set_size(bars, LV_SIZE_CONTENT, col_h);
  lv_obj_set_style_pad_column(bars, gap, 0);
  lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);  // bottoms level
  for (int i = 0; i < 4; ++i) {
    lv_obj_t *bar = makeBox(bars);
    int32_t bar_h = col_h * (i + 1) / 4;
    if (bar_h < 2) bar_h = 2;
    lv_obj_set_size(bar, bar_w, bar_h);
  }
  setWifiBars(bars, 0, colorText(), colorPanelBg());
  return bars;
}

void setWifiBars(lv_obj_t *bars, int count, lv_color_t on, lv_color_t bg) {
  lv_color_t off = lv_color_mix(on, bg, 64);  // a quarter of the way from the background to `on`
  for (int i = 0; i < 4; ++i) {
    lv_obj_t *bar = lv_obj_get_child(bars, i);
    if (bar == nullptr) break;
    lv_obj_set_style_bg_color(bar, i < count ? on : off, 0);
  }
}

}  // namespace transit_app::ui
