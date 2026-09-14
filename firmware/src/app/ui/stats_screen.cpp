#include "stats_screen.h"

#include <vector>

#include "../net_poller.h"
#include "transit_stats/summary.h"
#include "ui_common.h"

namespace transit_app::ui {

namespace {

struct StatsRow {
  std::string stop_key;
  std::string label;  // StopConfig::label, e.g. "17 Southbound" - the fixed prefix for this row
  lv_obj_t *widget;
};

struct StatsScreenCtx {
  std::vector<StatsRow> rows;
};

void formatRow(lv_obj_t *label_widget, const std::string &prefix, const transit_stats::StopSummary &s) {
  if (s.samples == 0) {
    lv_label_set_text_fmt(label_widget, "%s: no data yet", prefix.c_str());
    return;
  }
  if (s.worst_hour >= 0) {
    lv_label_set_text_fmt(label_widget, "%s: %.0f%% on-time, %+.1fm avg, worst %dh, %u ghosts (n=%u)", prefix.c_str(),
                           (double)s.on_time_pct, (double)s.mean_late_min, (int)s.worst_hour, (unsigned)s.ghosts,
                           (unsigned)s.samples);
  } else {
    lv_label_set_text_fmt(label_widget, "%s: %.0f%% on-time, %+.1fm avg, %u ghosts (n=%u)", prefix.c_str(),
                           (double)s.on_time_pct, (double)s.mean_late_min, (unsigned)s.ghosts, (unsigned)s.samples);
  }
}

}  // namespace

lv_obj_t *createStatsScreen(const Config &cfg) {
  int32_t w, h;
  screenSize(w, h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_style_pad_all(screen, 10, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(screen, 6, 0);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *title = lv_label_create(screen);
  lv_obj_set_style_text_font(title, fontBody(h), 0);
  lv_obj_set_style_text_color(title, colorText(), 0);
  lv_label_set_text(title, "Stats - last 30 days");

  auto *ctx = new StatsScreenCtx();

  if (cfg.stops.empty()) {
    lv_obj_t *empty = lv_label_create(screen);
    lv_obj_set_style_text_font(empty, fontSmall(h), 0);
    lv_obj_set_style_text_color(empty, colorSubtext(), 0);
    lv_label_set_text(empty, "No stops configured yet.");
  }

  for (const transit::StopConfig &s : cfg.stops) {
    lv_obj_t *row = lv_label_create(screen);
    lv_obj_set_style_text_font(row, fontSmall(h), 0);
    lv_obj_set_style_text_color(row, colorText(), 0);
    lv_obj_set_width(row, lv_pct(100));
    lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
    std::string prefix = s.label.empty() ? s.key : s.label;
    lv_label_set_text_fmt(row, "%s: --", prefix.c_str());
    ctx->rows.push_back({s.key, prefix, row});
  }

  lv_obj_set_user_data(screen, ctx);
  return screen;
}

void refreshStatsScreen(lv_obj_t *screen) {
  auto *ctx = static_cast<StatsScreenCtx *>(lv_obj_get_user_data(screen));
  if (ctx == nullptr) {
    return;
  }
  for (const StatsRow &row : ctx->rows) {
    transit_stats::StopSummary summary;
    if (getStopSummary(row.stop_key, summary)) {
      formatRow(row.widget, row.label, summary);
    } else {
      lv_label_set_text_fmt(row.widget, "%s: no data yet", row.label.c_str());
    }
  }
}

}  // namespace transit_app::ui
