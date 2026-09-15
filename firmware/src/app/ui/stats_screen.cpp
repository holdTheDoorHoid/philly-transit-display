#include "stats_screen.h"

#include <cstdio>
#include <string>
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

void formatRow(lv_obj_t *label_widget, const std::string &prefix, const StopSummaryView &v) {
  // F27: the summary is computed on the poller task now, so the first visit to this page finds
  // nothing cached. Say so. Rendering StopSummary's default zeroes here would read as "0% on
  // time, no samples", which is a claim, not a blank.
  if (!v.has_value) {
    lv_label_set_text_fmt(label_widget, "%s: loading\xE2\x80\xA6", prefix.c_str());
    return;
  }
  const transit_stats::StopSummary &s = v.summary;
  if (s.samples == 0) {
    lv_label_set_text_fmt(label_widget, "%s: no data yet", prefix.c_str());
    return;
  }

  char buf[96];
  std::string body;
  if (s.has_on_time) {
    snprintf(buf, sizeof buf, "%.0f%% on-time, %+.1fm avg", (double)s.on_time_pct, (double)s.mean_late_min);
  } else {
    // DESIGN.md SS9.2 (F21): with no arrival whose lateness we ever learned there is no
    // percentage. A dash is the answer; "0%" would be the worst possible one.
    snprintf(buf, sizeof buf, "on-time --");
  }
  body = buf;
  if (s.worst_hour >= 0) {
    snprintf(buf, sizeof buf, ", worst %dh", (int)s.worst_hour);
    body += buf;
  }
  // "N inferred" is not a footnote: every `arrive` row in this log is derived from a prediction
  // that stopped being published, never from a measured passage (DESIGN.md SS9.2), and the page
  // has to say so next to the numbers built on it.
  snprintf(buf, sizeof buf, ", %u ghosts (n=%u, %u inferred)", (unsigned)s.ghosts, (unsigned)s.samples,
           (unsigned)v.inferred);
  body += buf;
  lv_label_set_text_fmt(label_widget, "%s: %s", prefix.c_str(), body.c_str());
}

}  // namespace

lv_obj_t *createStatsScreen(const Config &cfg) {
  int32_t w, h;
  screenSize(w, h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
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
  lv_obj_add_event_cb(screen, [](lv_event_t *e) {  // see main_screen.cpp: freed with the screen
    lv_obj_t *scr = static_cast<lv_obj_t *>(lv_event_get_target(e));
    delete static_cast<StatsScreenCtx *>(lv_obj_get_user_data(scr));
    lv_obj_set_user_data(scr, nullptr);
  }, LV_EVENT_DELETE, nullptr);
  return screen;
}

void refreshStatsScreen(lv_obj_t *screen) {
  auto *ctx = static_cast<StatsScreenCtx *>(lv_obj_get_user_data(screen));
  if (ctx == nullptr) {
    return;
  }
  // getStopSummary() never touches SD from here any more (F27) - it returns the cached value and
  // asks the poller task for a refresh - so this is safe to call at the screen's own cadence.
  for (const StatsRow &row : ctx->rows) {
    formatRow(row.widget, row.label, getStopSummary(row.stop_key));
  }
}

}  // namespace transit_app::ui
