#include "stats_screen.h"

#include "ui_common.h"

namespace transit_app::ui {

lv_obj_t *createStatsScreen() {
  int32_t w, h;
  screenSize(w, h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *title = lv_label_create(screen);
  lv_obj_set_style_text_font(title, fontBody(h), 0);
  lv_obj_set_style_text_color(title, colorText(), 0);
  lv_label_set_text(title, "Stats");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, h / 4);

  lv_obj_t *body = lv_label_create(screen);
  lv_obj_set_style_text_font(body, fontSmall(h), 0);
  lv_obj_set_style_text_color(body, colorSubtext(), 0);
  lv_obj_set_width(body, lv_pct(80));
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_label_set_text(body, "Coming soon: on-time %, mean late, worst hour,\nghost count and sample count per stop\n(see transit_stats).");
  lv_obj_align_to(body, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);

  return screen;
}

}  // namespace transit_app::ui
