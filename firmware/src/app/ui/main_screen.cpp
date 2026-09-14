// Panel/row *structure* comes from `cfg.stops` (real config) rather than from the Snapshot: the
// config says which stops and how many rows each gets; the Snapshot (net_poller's real one, or
// demo_data.h's hardcoded one under -DDEMO_DATA - see ui.cpp's currentSnapshot()) supplies the
// numbers that fill those rows in. A configured stop with no matching StopSnapshot::key in the
// current Snapshot, or one whose poll failed, renders a "no data"/"poll failed" row instead of
// stale or fabricated numbers (DESIGN.md SS8).
#include "main_screen.h"

#include <WiFi.h>

#include <cstdio>
#include <ctime>
#include <vector>

#include "ui_common.h"

using transit::Arrival;
using transit::Snapshot;
using transit::StopConfig;
using transit::StopSnapshot;

namespace transit_app::ui {

namespace {

struct RowWidgets {
  lv_obj_t *route_badge;
  lv_obj_t *destination;
  lv_obj_t *minutes;
  lv_obj_t *status_badge;
};

struct PanelWidgets {
  std::string stop_key;
  std::string route;
  lv_obj_t *title;
  lv_obj_t *no_data_label;
  std::vector<RowWidgets> rows;
};

struct MainScreenCtx {
  lv_obj_t *header;
  lv_obj_t *device_label;
  lv_obj_t *clock_label;
  lv_obj_t *wifi_label;
  lv_obj_t *updated_label;
  lv_obj_t *ticker;
  std::vector<PanelWidgets> panels;
};

std::string panelTitle(const StopConfig &s) {
  char buf[96];
  if (s.mode == transit::Mode::Rail) {
    snprintf(buf, sizeof(buf), "%s%s%s", s.station.c_str(), s.direction.empty() ? "" : " ", s.direction.empty() ? "" : ("(" + s.direction + ")").c_str());
  } else {
    snprintf(buf, sizeof(buf), "%s %s %s %s %s", s.route.c_str(), LV_SYMBOL_RIGHT, s.headsign.c_str(), "\xC2\xB7" /* middle dot */, s.stop_name.c_str());
  }
  std::string title(buf);
  // DESIGN.md SS4.6/SS8: subway has no realtime source in v1 (mergeStop() falls back to
  // BusSchedules-only, Status::Scheduled for every row) - the panel says so up front rather than
  // making the user infer it from every row showing "sched".
  if (s.mode == transit::Mode::Subway) {
    title += " \xC2\xB7 schedule only";
  }
  return title;
}

const StopSnapshot *findStopSnapshot(const Snapshot &snap, const std::string &key) {
  for (const StopSnapshot &s : snap.stops) {
    if (s.key == key) {
      return &s;
    }
  }
  return nullptr;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

}  // namespace

lv_obj_t *createMainScreen(const Config &cfg) {
  int32_t w, h;
  screenSize(w, h);
  int rows = rowsPerStop(h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);  // DESIGN.md SS8: tap anywhere cycles pages

  auto *ctx = new MainScreenCtx();

  // ---- Header (10% height) ----
  int32_t header_h = h / 10 > 20 ? h / 10 : 20;
  lv_obj_t *header = lv_obj_create(screen);
  lv_obj_set_size(header, lv_pct(100), header_h);
  lv_obj_set_style_bg_color(header, colorPanelBg(), 0);
  lv_obj_set_style_pad_hor(header, 8, 0);
  lv_obj_set_style_pad_ver(header, 2, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  ctx->header = header;

  ctx->device_label = makeLabel(header, fontSmall(h), colorText());
  lv_label_set_text(ctx->device_label, cfg.device.name.c_str());

  ctx->clock_label = makeLabel(header, fontSmall(h), colorText());
  lv_label_set_text(ctx->clock_label, "--:--");

  lv_obj_t *right_group = lv_obj_create(header);
  lv_obj_remove_flag(right_group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(right_group, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(right_group, 0, 0);
  lv_obj_set_style_pad_all(right_group, 0, 0);
  lv_obj_set_size(right_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(right_group, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right_group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(right_group, 8, 0);

  ctx->wifi_label = makeLabel(right_group, fontSmall(h), colorSubtext());
  lv_label_set_text(ctx->wifi_label, LV_SYMBOL_WIFI);

  ctx->updated_label = makeLabel(right_group, fontSmall(h), colorSubtext());
  lv_label_set_text(ctx->updated_label, "updated -- ago");

  // ---- Stop panels ----
  lv_obj_t *panels_area = lv_obj_create(screen);
  lv_obj_set_size(panels_area, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_grow(panels_area, 1);
  lv_obj_set_style_bg_opa(panels_area, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(panels_area, 0, 0);
  lv_obj_set_style_pad_all(panels_area, 4, 0);
  lv_obj_set_style_pad_row(panels_area, 4, 0);
  lv_obj_set_flex_flow(panels_area, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(panels_area, LV_OBJ_FLAG_SCROLLABLE);

  for (const StopConfig &s : cfg.stops) {
    PanelWidgets pw;
    pw.stop_key = s.key;
    pw.route = s.route;

    lv_obj_t *panel = lv_obj_create(panels_area);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_bg_color(panel, colorPanelBg(), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    pw.title = makeLabel(panel, fontBody(h), colorText());
    lv_label_set_text(pw.title, panelTitle(s).c_str());
    lv_obj_set_width(pw.title, lv_pct(100));
    lv_label_set_long_mode(pw.title, LV_LABEL_LONG_DOT);

    pw.no_data_label = makeLabel(panel, fontSmall(h), colorSubtext());
    lv_label_set_text(pw.no_data_label, "no data yet");
    lv_obj_add_flag(pw.no_data_label, LV_OBJ_FLAG_HIDDEN);

    for (int r = 0; r < rows; ++r) {
      lv_obj_t *row = lv_obj_create(panel);
      lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(row, 0, 0);
      lv_obj_set_style_pad_all(row, 2, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 6, 0);
      lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

      RowWidgets rw;
      rw.route_badge = makeLabel(row, fontSmall(h), lv_color_white());
      lv_obj_set_style_bg_color(rw.route_badge, routeBadgeColor(), 0);
      lv_obj_set_style_bg_opa(rw.route_badge, LV_OPA_COVER, 0);
      lv_obj_set_style_pad_hor(rw.route_badge, 4, 0);
      lv_obj_set_style_radius(rw.route_badge, 4, 0);

      rw.destination = makeLabel(row, fontBody(h), colorText());
      lv_obj_set_flex_grow(rw.destination, 1);
      lv_label_set_long_mode(rw.destination, LV_LABEL_LONG_DOT);

      rw.minutes = makeLabel(row, fontBig(h), colorText());
      lv_obj_set_style_text_align(rw.minutes, LV_TEXT_ALIGN_RIGHT, 0);

      rw.status_badge = makeLabel(row, fontSmall(h), colorSubtext());

      pw.rows.push_back(rw);
    }

    ctx->panels.push_back(pw);
  }

  // ---- Footer ticker (only shown when refreshMainScreen finds alerts) ----
  ctx->ticker = lv_label_create(screen);
  lv_obj_set_width(ctx->ticker, lv_pct(100));
  lv_obj_set_style_text_font(ctx->ticker, fontSmall(h), 0);
  lv_obj_set_style_text_color(ctx->ticker, colorText(), 0);
  lv_obj_set_style_bg_color(ctx->ticker, colorPanelBg(), 0);
  lv_obj_set_style_bg_opa(ctx->ticker, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(ctx->ticker, 4, 0);
  lv_label_set_long_mode(ctx->ticker, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(ctx->ticker, "");
  lv_obj_add_flag(ctx->ticker, LV_OBJ_FLAG_HIDDEN);

  lv_obj_set_user_data(screen, ctx);
  return screen;
}

void refreshMainScreen(lv_obj_t *screen, const Config &cfg, const Snapshot &snap) {
  auto *ctx = static_cast<MainScreenCtx *>(lv_obj_get_user_data(screen));
  if (ctx == nullptr) {
    return;
  }

  lv_label_set_text(ctx->device_label, cfg.device.name.c_str());  // may change via PUT /api/config

  time_t now = time(nullptr);
  struct tm local_tm;
  localtime_r(&now, &local_tm);
  char clock_buf[16];
  int hour12 = local_tm.tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  snprintf(clock_buf, sizeof(clock_buf), "%d:%02d %s", hour12, local_tm.tm_min, local_tm.tm_hour < 12 ? "AM" : "PM");
  lv_label_set_text(ctx->clock_label, clock_buf);

  // Wi-Fi bars: fold RSSI into a rough 0-3 "bars" count next to the symbol.
  int32_t rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100;
  const char *bars = rssi > -60 ? "3" : rssi > -75 ? "2" : rssi > -90 ? "1" : "0";
  lv_label_set_text_fmt(ctx->wifi_label, "%s %s", LV_SYMBOL_WIFI, bars);

  int32_t age_s = snap.generated > 0 ? (int32_t)((int64_t)now - snap.generated) : -1;
  bool stale = age_s > 90;  // DESIGN.md SS8: header turns amber when stale
  if (age_s < 0) {
    lv_label_set_text(ctx->updated_label, "updated --");
  } else if (stale) {
    // DESIGN.md SS8: "header turns amber with 'stale 4 min'".
    lv_label_set_text_fmt(ctx->updated_label, "stale %ld min", (long)(age_s / 60));
  } else {
    lv_label_set_text_fmt(ctx->updated_label, "updated %ld s ago", (long)age_s);
  }
  lv_obj_set_style_bg_color(ctx->header, stale ? colorStale() : colorPanelBg(), 0);

  for (PanelWidgets &pw : ctx->panels) {
    const StopSnapshot *stop = findStopSnapshot(snap, pw.stop_key);
    bool have_data = stop != nullptr && stop->ok && !stop->arrivals.empty();
    if (have_data) {
      lv_obj_add_flag(pw.no_data_label, LV_OBJ_FLAG_HIDDEN);
    } else {
      // DESIGN.md SS8: "no data: panel shows the reason" - distinguish "the whole poll failed"
      // from "this stop just has nothing upcoming right now" rather than one generic message.
      if (stop == nullptr) {
        lv_label_set_text(pw.no_data_label, "no data");
      } else if (!snap.last_poll_ok) {
        lv_label_set_text(pw.no_data_label, snap.last_error.empty() ? "poll failed" : ("poll failed: " + snap.last_error).c_str());
      } else if (!stop->ok) {
        lv_label_set_text(pw.no_data_label, stop->error.empty() ? "unavailable" : stop->error.c_str());
      } else {
        lv_label_set_text(pw.no_data_label, "no arrivals");
      }
      lv_obj_remove_flag(pw.no_data_label, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < pw.rows.size(); ++i) {
      RowWidgets &rw = pw.rows[i];
      bool row_hidden = !have_data || i >= stop->arrivals.size();
      lv_obj_t *row_parent = lv_obj_get_parent(rw.route_badge);
      if (row_hidden) {
        lv_obj_add_flag(row_parent, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_remove_flag(row_parent, LV_OBJ_FLAG_HIDDEN);

      const Arrival &a = stop->arrivals[i];
      lv_label_set_text(rw.route_badge, pw.route.c_str());
      lv_label_set_text(rw.destination, a.destination.c_str());

      transit::Epoch eff = a.effective();
      transit::Epoch eta_s = eff > 0 ? (eff - (transit::Epoch)now) : 0;
      lv_label_set_text(rw.minutes, minutesLabel(eta_s).c_str());

      Badge badge = badgeFor(a);
      lv_label_set_text(rw.status_badge, badge.text.c_str());
      lv_obj_set_style_text_color(rw.status_badge, badge.color, 0);
    }
  }

  if (snap.alerts.empty()) {
    lv_obj_add_flag(ctx->ticker, LV_OBJ_FLAG_HIDDEN);
  } else {
    std::string text;
    for (const transit::Alert &al : snap.alerts) {
      if (!text.empty()) {
        text += "   \xE2\x80\xA2   ";  // " • "
      }
      text += al.route + ": " + al.text;
    }
    lv_label_set_text(ctx->ticker, text.c_str());
    lv_obj_remove_flag(ctx->ticker, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace transit_app::ui
