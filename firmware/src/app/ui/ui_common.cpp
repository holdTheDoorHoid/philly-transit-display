#include "ui_common.h"

#include <cstdio>
#include <ctime>

namespace transit_app::ui {

namespace {
bool g_dark = false;
}  // namespace

void setTheme(const std::string &name) {
  g_dark = (name == "dark");
}

bool isDarkTheme() {
  return g_dark;
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

int rowsPerStop(int32_t h) {
  return h >= 320 ? 3 : 2;
}

const lv_font_t *fontBig(int32_t h) {
#if LV_FONT_MONTSERRAT_20
  if (h < 320) return &lv_font_montserrat_20;  // 240-tall panels (2.4"/2.8" boards)
#else
  (void)h;
#endif
  return &lv_font_montserrat_28;
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

}  // namespace transit_app::ui
