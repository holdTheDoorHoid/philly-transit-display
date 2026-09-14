#include "ui_common.h"

namespace transit_app::ui {

lv_color_t colorBg() {
  return lv_color_hex(0x101418);
}
lv_color_t colorPanelBg() {
  return lv_color_hex(0x1c2126);
}
lv_color_t colorText() {
  return lv_color_hex(0xf0f0f0);
}
lv_color_t colorSubtext() {
  return lv_color_hex(0x9aa4ad);
}
lv_color_t colorOnTime() {
  return lv_color_hex(0x2fb344);
}
lv_color_t colorLate() {
  return lv_color_hex(0xe0403f);
}
lv_color_t colorEarly() {
  return lv_color_hex(0x3b82f6);
}
lv_color_t colorScheduled() {
  return lv_color_hex(0x8a8f96);
}
lv_color_t colorSkipped() {
  return lv_color_hex(0xf08c1a);
}
lv_color_t colorStale() {
  return lv_color_hex(0xf0a500);
}
lv_color_t routeBadgeColor() {
  return lv_color_hex(0x274b8f);  // SEPTA-ish blue
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
  return h >= 320 ? &lv_font_montserrat_28 : &lv_font_montserrat_20;
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
