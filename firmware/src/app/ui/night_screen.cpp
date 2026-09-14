#include "night_screen.h"

#include <ctime>
#include <string>
#include <vector>

#include "../profiles.h"
#include "../weather_service.h"
#include "ui_common.h"

namespace transit_app::ui {

namespace {

struct NightRow {
  std::string stop_key;
  std::string label;
  lv_obj_t *widget;
};

struct NightCtx {
  lv_obj_t *clock_label;
  lv_obj_t *date_label;
  lv_obj_t *weather_label;
  std::vector<NightRow> rows;
};

const transit::StopSnapshot *findStop(const transit::Snapshot &snap, const std::string &key) {
  for (const transit::StopSnapshot &s : snap.stops) {
    if (s.key == key) return &s;
  }
  return nullptr;
}

}  // namespace

bool nightConditionMet(const Config &cfg, const transit::Snapshot &snap, const std::vector<std::string> &shown_keys,
                       transit::Epoch now) {
  if (!cfg.device.night.enabled || snap.generated == 0 || shown_keys.empty()) return false;
  transit::Epoch horizon = (transit::Epoch)cfg.device.night.after_min * 60;
  for (const std::string &key : shown_keys) {
    const transit::StopConfig *sc = nullptr;
    for (const transit::StopConfig &s : cfg.stops) {
      if (s.key == key) {
        sc = &s;
        break;
      }
    }
    if (sc == nullptr || !sc->alt_of.empty()) continue;  // alternatives don't keep the lights on
    const transit::StopSnapshot *st = findStop(snap, key);
    if (st == nullptr || !st->ok) return false;
    for (const transit::Arrival &a : st->arrivals) {
      transit::Epoch eff = a.effective();
      if (eff > 0 && eff - now < horizon) return false;
    }
  }
  return true;
}

lv_obj_t *createNightScreen(const Config &cfg) {
  int32_t w, h;
  screenSize(w, h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_style_pad_all(screen, 12, 0);
  lv_obj_set_style_pad_row(screen, 6, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  auto *ctx = new NightCtx();

  ctx->clock_label = lv_label_create(screen);
  lv_obj_set_style_text_font(ctx->clock_label, fontHuge(), 0);
  lv_obj_set_style_text_color(ctx->clock_label, colorText(), 0);
  lv_label_set_text(ctx->clock_label, "--:--");

  ctx->date_label = lv_label_create(screen);
  lv_obj_set_style_text_font(ctx->date_label, fontBody(h), 0);
  lv_obj_set_style_text_color(ctx->date_label, colorSubtext(), 0);
  lv_label_set_text(ctx->date_label, "");

  ctx->weather_label = lv_label_create(screen);
  lv_obj_set_style_text_font(ctx->weather_label, fontBody(h), 0);
  lv_obj_set_style_text_color(ctx->weather_label, colorText(), 0);
  lv_label_set_text(ctx->weather_label, "");
  lv_obj_set_style_pad_bottom(ctx->weather_label, 10, 0);

  for (const transit::StopConfig &s : visibleStops(cfg, time(nullptr))) {
    if (!s.alt_of.empty()) continue;
    lv_obj_t *row = lv_label_create(screen);
    lv_obj_set_style_text_font(row, fontBody(h), 0);
    lv_obj_set_style_text_color(row, colorText(), 0);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
    std::string label = s.label.empty() ? s.route : s.label;
    lv_label_set_text_fmt(row, "%s: --", label.c_str());
    ctx->rows.push_back({s.key, label, row});
  }

  lv_obj_t *hint = lv_label_create(screen);
  lv_obj_set_style_text_font(hint, fontSmall(h), 0);
  lv_obj_set_style_text_color(hint, colorSubtext(), 0);
  lv_obj_set_style_pad_top(hint, 10, 0);
  lv_label_set_text(hint, "nothing due soon \xE2\x80\xA2 tap for details");

  lv_obj_set_user_data(screen, ctx);
  lv_obj_add_event_cb(screen, [](lv_event_t *e) {
    lv_obj_t *scr = static_cast<lv_obj_t *>(lv_event_get_target(e));
    delete static_cast<NightCtx *>(lv_obj_get_user_data(scr));
    lv_obj_set_user_data(scr, nullptr);
  }, LV_EVENT_DELETE, nullptr);
  return screen;
}

void refreshNightScreen(lv_obj_t *screen, const Config &cfg, const transit::Snapshot &snap) {
  auto *ctx = static_cast<NightCtx *>(lv_obj_get_user_data(screen));
  if (ctx == nullptr) return;
  (void)cfg;

  time_t now = time(nullptr);
  lv_label_set_text(ctx->clock_label, clockLabel((transit::Epoch)now).c_str());
  struct tm lt;
  localtime_r(&now, &lt);
  char date_buf[48];
  strftime(date_buf, sizeof(date_buf), "%A, %B %e", &lt);
  lv_label_set_text(ctx->date_label, date_buf);
  lv_label_set_text(ctx->weather_label, headerWeatherText().c_str());

  for (NightRow &row : ctx->rows) {
    const transit::StopSnapshot *st = findStop(snap, row.stop_key);
    const transit::Arrival *next = nullptr;
    if (st != nullptr) {
      for (const transit::Arrival &a : st->arrivals) {
        if (a.effective() > 0 && a.effective() >= (transit::Epoch)now - 60) {
          next = &a;
          break;
        }
      }
    }
    if (next == nullptr) {
      lv_label_set_text_fmt(row.widget, "%s: no departures listed", row.label.c_str());
    } else {
      lv_label_set_text_fmt(row.widget, "%s: next %s%s", row.label.c_str(), clockLabel(next->effective()).c_str(),
                            next->status == transit::Status::Scheduled ? " (sched)" : "");
    }
  }
}

}  // namespace transit_app::ui
