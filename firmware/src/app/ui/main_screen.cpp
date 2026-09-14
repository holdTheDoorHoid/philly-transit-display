// Panel/row *structure* comes from `cfg.stops` (real config) rather than from the Snapshot: the
// config says which stops and how many rows each gets; the Snapshot (net_poller's real one, or
// demo_data.h's hardcoded one under -DDEMO_DATA - see ui.cpp's currentSnapshot()) supplies the
// numbers that fill those rows in. A configured stop with no matching StopSnapshot::key in the
// current Snapshot, or one whose poll failed, renders a "no data"/"poll failed" row instead of
// stale or fabricated numbers (DESIGN.md SS8).
#include "main_screen.h"

#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "../bike_service.h"
#include "../due_alert.h"
#include "../profiles.h"
#include "../weather_service.h"
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
  lv_obj_t *crowd_icons;  // three-slot chair/person meter (device.crowding icons|both), hidden when unknown
  lv_obj_t *crowding;     // "few seats" etc. (device.crowding words|both), hidden when unknown
  lv_obj_t *minutes;
  lv_obj_t *status_badge;
};

struct PanelWidgets {
  std::string stop_key;
  std::string route;
  std::string alt_of;       // DESIGN.md SS6: shown only while this stop's next arrival is far out
  uint8_t alt_after_min = 15;
  lv_obj_t *panel;
  lv_obj_t *title;
  lv_obj_t *weather_note;  // DESIGN.md SS8: shown only when the forecast at the next arrival is notable
  lv_obj_t *no_data_label;
  std::vector<RowWidgets> rows;
};

struct MainScreenCtx {
  lv_obj_t *header;
  lv_obj_t *device_label;
  lv_obj_t *clock_label;
  lv_obj_t *weather_label;
  lv_obj_t *wifi_label;
  lv_obj_t *updated_label;
  bool header_stale = false;
  // Alert ticker (DESIGN.md SS8): a clipping box with one label inside, moved by an lv_anim at
  // a fixed pixel speed. LVGL's built-in label scroll caps one full pass at 10 s regardless of
  // length, which for a paragraph of SEPTA detour text was unreadably fast.
  lv_obj_t *ticker_box;
  lv_obj_t *ticker_label;
  std::string ticker_text;
  std::string ticker_show = "both";  // config.device.ticker_show
  int ticker_lines = 1;
  uint16_t ticker_speed = 30;
  // Indego strip (DESIGN.md SS4.9): one label per configured station, above the ticker.
  lv_obj_t *bike_box;
  std::vector<lv_obj_t *> bike_labels;
  bool blink_phase = false;  // due-row blink (due_alert.h)
  std::vector<PanelWidgets> panels;
};

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

// Plain container: no theme chrome, and not clickable so a tap on it reaches the screen's
// tap-to-cycle handler (DESIGN.md SS8 "tap anywhere") instead of stopping at the child.
lv_obj_t *makeBox(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  // LVGL's default bg_opa is 0 (no theme is compiled in): a bg_color alone paints nothing.
  // The first builds set colours without this, which is why the "background" never changed.
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

void animSetX(void *obj, int32_t v) {
  lv_obj_set_x(static_cast<lv_obj_t *>(obj), v);
}
void animSetY(void *obj, int32_t v) {
  lv_obj_set_y(static_cast<lv_obj_t *>(obj), v);
}

// Ticker copy: "17: <alert>" then "17 detour: <detour>" for each distinct detour (SEPTA lists
// the same one twice), one item per line when the ticker is taller than a line, otherwise joined
// with " • " for the single-line marquee.
std::string tickerText(const std::vector<transit::Alert> &alerts, bool multiline, const std::string &mode) {
  if (mode == "off") return "";
  bool want_alerts = (mode == "both" || mode == "alerts");
  bool want_detours = (mode == "both" || mode == "detours");
  const char *sep = multiline ? "\n" : "   \xE2\x80\xA2   ";
  std::string text;
  std::vector<std::string> seen;
  auto add = [&](const std::string &item) {
    if (item.empty()) return;
    for (const std::string &s : seen) {
      if (s == item) return;
    }
    seen.push_back(item);
    if (!text.empty()) text += sep;
    text += item;
  };
  for (const transit::Alert &al : alerts) {
    if (want_alerts) add(al.route + ": " + al.text);
    if (want_detours) {
      for (const std::string &d : al.detours) add(al.route + " detour: " + d);
    }
  }
  return text;
}

// (Re)starts the ticker animation for the current text: a horizontal marquee for a one-line
// ticker, a credits-style upward scroll for a taller one; static when the text already fits.
// The whole pass runs at config.device.ticker_speed pixels per second.
void restartTicker(MainScreenCtx *ctx) {
  lv_obj_t *box = ctx->ticker_box;
  lv_obj_t *label = ctx->ticker_label;
  lv_anim_delete(label, nullptr);
  lv_obj_update_layout(box);
  int32_t box_w = lv_obj_get_content_width(box);
  int32_t box_h = lv_obj_get_content_height(box);
  int32_t label_w = lv_obj_get_width(label);
  int32_t label_h = lv_obj_get_height(label);
  uint32_t speed = std::max<uint32_t>(5, std::min<uint32_t>(ctx->ticker_speed, 200));

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, label);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  if (ctx->ticker_lines <= 1) {
    lv_obj_set_y(label, 0);
    if (label_w <= box_w) {
      lv_obj_set_x(label, 0);
      return;
    }
    lv_anim_set_exec_cb(&a, animSetX);
    lv_anim_set_values(&a, box_w, -label_w);
    lv_anim_set_duration(&a, (uint32_t)(box_w + label_w) * 1000u / speed);
  } else {
    lv_obj_set_x(label, 0);
    if (label_h <= box_h) {
      lv_obj_set_y(label, 0);
      return;
    }
    lv_anim_set_exec_cb(&a, animSetY);
    lv_anim_set_values(&a, box_h, -label_h);
    lv_anim_set_duration(&a, (uint32_t)(box_h + label_h) * 1000u / speed);
  }
  lv_anim_start(&a);
}

}  // namespace

lv_obj_t *createMainScreen(const Config &cfg) {
  int32_t w, h;
  screenSize(w, h);
  bool large = cfg.device.large_text;
  int rows = rowsPerStop(h, large);
  const lv_font_t *minutes_font = large ? fontHuge() : fontBig(h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);  // DESIGN.md SS8: tap anywhere cycles pages

  auto *ctx = new MainScreenCtx();

  // ---- Header (10% height) ----
  int32_t header_h = h / 10 > 20 ? h / 10 : 20;
  lv_obj_t *header = makeBox(screen);
  lv_obj_set_size(header, lv_pct(100), header_h);
  lv_obj_set_style_bg_color(header, colorPanelBg(), 0);
  lv_obj_set_style_pad_hor(header, 8, 0);
  lv_obj_set_style_pad_ver(header, 2, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  ctx->header = header;

  ctx->device_label = makeLabel(header, fontSmall(h), colorText());
  lv_label_set_text(ctx->device_label, cfg.device.name.c_str());

  ctx->clock_label = makeLabel(header, fontSmall(h), colorText());
  lv_label_set_text(ctx->clock_label, "--:--");

  ctx->weather_label = makeLabel(header, fontSmall(h), colorText());
  lv_label_set_text(ctx->weather_label, "");

  lv_obj_t *right_group = makeBox(header);
  lv_obj_set_style_bg_opa(right_group, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(right_group, 0, 0);
  lv_obj_set_size(right_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(right_group, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right_group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(right_group, 8, 0);

  ctx->wifi_label = makeLabel(right_group, fontSmall(h), colorSubtext());
  lv_label_set_text(ctx->wifi_label, LV_SYMBOL_WIFI);

  ctx->updated_label = makeLabel(right_group, fontSmall(h), colorSubtext());
  lv_label_set_text(ctx->updated_label, "updated -- ago");

  // DESIGN.md SS8: config.device.header picks what the (narrow) header shows; hidden flex items
  // take no space, so the remaining ones spread out.
  const HeaderConfig &hc = cfg.device.header;
  if (!hc.name) lv_obj_add_flag(ctx->device_label, LV_OBJ_FLAG_HIDDEN);
  if (!hc.clock) lv_obj_add_flag(ctx->clock_label, LV_OBJ_FLAG_HIDDEN);
  if (!hc.weather || !cfg.weather.enabled) lv_obj_add_flag(ctx->weather_label, LV_OBJ_FLAG_HIDDEN);
  if (!hc.wifi) lv_obj_add_flag(ctx->wifi_label, LV_OBJ_FLAG_HIDDEN);
  if (!hc.updated) lv_obj_add_flag(ctx->updated_label, LV_OBJ_FLAG_HIDDEN);

  // ---- Stop panels ----
  lv_obj_t *panels_area = makeBox(screen);
  lv_obj_set_size(panels_area, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_grow(panels_area, 1);
  lv_obj_set_style_bg_opa(panels_area, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(panels_area, 4, 0);
  lv_obj_set_style_pad_row(panels_area, 4, 0);
  lv_obj_set_flex_flow(panels_area, LV_FLEX_FLOW_COLUMN);

  for (const StopConfig &s : visibleStops(cfg, time(nullptr))) {
    PanelWidgets pw;
    pw.stop_key = s.key;
    pw.route = s.route;
    pw.alt_of = s.alt_of;
    pw.alt_after_min = s.alt_after_min;

    lv_obj_t *panel = makeBox(panels_area);
    pw.panel = panel;
    if (!s.alt_of.empty()) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);  // until the primary runs late
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_bg_color(panel, colorPanelBg(), 0);
    // Square corners: LV_DRAW_SW_COMPLEX is 0 (lv_conf.h, flash budget) and LVGL then skips a
    // rounded rectangle entirely rather than drawing it square.
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    pw.title = makeLabel(panel, fontBody(h), colorText());
    lv_label_set_text(pw.title, panelTitle(s).c_str());
    lv_obj_set_width(pw.title, lv_pct(100));
    lv_label_set_long_mode(pw.title, LV_LABEL_LONG_DOT);

    pw.weather_note = makeLabel(panel, fontSmall(h), colorEarly());
    lv_obj_set_width(pw.weather_note, lv_pct(100));
    lv_label_set_long_mode(pw.weather_note, LV_LABEL_LONG_DOT);
    lv_label_set_text(pw.weather_note, "");
    lv_obj_add_flag(pw.weather_note, LV_OBJ_FLAG_HIDDEN);

    pw.no_data_label = makeLabel(panel, fontSmall(h), colorSubtext());
    lv_label_set_text(pw.no_data_label, "no data yet");
    lv_obj_add_flag(pw.no_data_label, LV_OBJ_FLAG_HIDDEN);

    for (int r = 0; r < rows; ++r) {
      lv_obj_t *row = makeBox(panel);
      lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(row, 2, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 6, 0);

      RowWidgets rw;
      rw.route_badge = makeLabel(row, fontSmall(h), lv_color_white());
      lv_obj_set_style_bg_color(rw.route_badge, routeBadgeColor(), 0);
      lv_obj_set_style_bg_opa(rw.route_badge, LV_OPA_COVER, 0);
      lv_obj_set_style_pad_hor(rw.route_badge, 4, 0);

      rw.destination = makeLabel(row, fontBody(h), colorText());
      lv_obj_set_flex_grow(rw.destination, 1);
      lv_label_set_long_mode(rw.destination, LV_LABEL_LONG_DOT);

      rw.crowd_icons = makeLabel(row, fontIcons(), colorSubtext());
      lv_label_set_recolor(rw.crowd_icons, true);  // crowdingIcons() colours each slot inline
      lv_obj_set_style_text_letter_space(rw.crowd_icons, 2, 0);
      lv_label_set_text(rw.crowd_icons, "");
      lv_obj_add_flag(rw.crowd_icons, LV_OBJ_FLAG_HIDDEN);

      rw.crowding = makeLabel(row, fontSmall(h), colorSubtext());
      lv_label_set_text(rw.crowding, "");
      lv_obj_add_flag(rw.crowding, LV_OBJ_FLAG_HIDDEN);

      rw.minutes = makeLabel(row, minutes_font, colorText());
      lv_obj_set_style_text_align(rw.minutes, LV_TEXT_ALIGN_RIGHT, 0);

      rw.status_badge = makeLabel(row, fontSmall(h), colorSubtext());

      pw.rows.push_back(rw);
    }

    ctx->panels.push_back(pw);
  }

  // ---- Indego strip (only shown when bike_service has stations) ----
  ctx->bike_box = makeBox(screen);
  lv_obj_set_size(ctx->bike_box, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(ctx->bike_box, colorPanelBg(), 0);
  lv_obj_set_style_pad_all(ctx->bike_box, 4, 0);
  lv_obj_set_style_pad_row(ctx->bike_box, 0, 0);
  lv_obj_set_flex_flow(ctx->bike_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(ctx->bike_box, LV_OBJ_FLAG_HIDDEN);
  for (size_t i = 0; i < kMaxBikeStations; ++i) {
    lv_obj_t *l = makeLabel(ctx->bike_box, fontSmall(h), colorText());
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, "");
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    ctx->bike_labels.push_back(l);
  }

  // ---- Alert ticker (only shown when refreshMainScreen finds alerts) ----
  ctx->ticker_lines = std::max<int>(1, std::min<int>(cfg.device.ticker_lines, 8));
  ctx->ticker_speed = cfg.device.ticker_speed;
  ctx->ticker_show = cfg.device.ticker_show;
  const lv_font_t *ticker_font = fontSmall(h);
  const int32_t ticker_pad = 4;
  const int32_t line_space = 2;
  int32_t ticker_h = ctx->ticker_lines * lv_font_get_line_height(ticker_font) + (ctx->ticker_lines - 1) * line_space + 2 * ticker_pad;

  ctx->ticker_box = makeBox(screen);
  lv_obj_set_size(ctx->ticker_box, lv_pct(100), ticker_h);
  lv_obj_set_style_bg_color(ctx->ticker_box, colorPanelBg(), 0);
  lv_obj_set_style_pad_all(ctx->ticker_box, ticker_pad, 0);
  lv_obj_add_flag(ctx->ticker_box, LV_OBJ_FLAG_HIDDEN);

  ctx->ticker_label = makeLabel(ctx->ticker_box, ticker_font, colorText());
  lv_obj_set_style_text_line_space(ctx->ticker_label, line_space, 0);
  if (ctx->ticker_lines <= 1) {
    lv_label_set_long_mode(ctx->ticker_label, LV_LABEL_LONG_CLIP);  // one line, as wide as its text
    lv_obj_set_width(ctx->ticker_label, LV_SIZE_CONTENT);
  } else {
    lv_label_set_long_mode(ctx->ticker_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->ticker_label, lv_pct(100));
  }
  lv_label_set_text(ctx->ticker_label, "");

  lv_obj_set_user_data(screen, ctx);
  // Freed with the screen (ui.cpp rebuildScreens() deletes and recreates screens on a config
  // change; without this each rebuild leaked the context - measured ~2 KB of heap per change).
  lv_obj_add_event_cb(screen, [](lv_event_t *e) {
    lv_obj_t *scr = static_cast<lv_obj_t *>(lv_event_get_target(e));
    delete static_cast<MainScreenCtx *>(lv_obj_get_user_data(scr));
    lv_obj_set_user_data(scr, nullptr);
  }, LV_EVENT_DELETE, nullptr);
  return screen;
}

void mainScreenDebug(lv_obj_t *screen, std::vector<std::string> &hidden_panels, std::string &ticker_text) {
  auto *ctx = static_cast<MainScreenCtx *>(lv_obj_get_user_data(screen));
  hidden_panels.clear();
  ticker_text.clear();
  if (ctx == nullptr) return;
  for (const PanelWidgets &pw : ctx->panels) {
    if (lv_obj_has_flag(pw.panel, LV_OBJ_FLAG_HIDDEN)) hidden_panels.push_back(pw.stop_key);
  }
  ticker_text = ctx->ticker_text;
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

  if (!lv_obj_has_flag(ctx->weather_label, LV_OBJ_FLAG_HIDDEN)) {
    lv_label_set_text(ctx->weather_label, headerWeatherText().c_str());
  }

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
  if (stale != ctx->header_stale) {
    ctx->header_stale = stale;
    lv_obj_set_style_bg_color(ctx->header, stale ? colorStale() : colorPanelBg(), 0);
    lv_color_t fg = stale ? colorOnStale() : colorText();
    lv_color_t sub = stale ? colorOnStale() : colorSubtext();
    lv_obj_set_style_text_color(ctx->device_label, fg, 0);
    lv_obj_set_style_text_color(ctx->clock_label, fg, 0);
    lv_obj_set_style_text_color(ctx->weather_label, fg, 0);
    lv_obj_set_style_text_color(ctx->wifi_label, sub, 0);
    lv_obj_set_style_text_color(ctx->updated_label, sub, 0);
  }

  ctx->blink_phase = !ctx->blink_phase;
  for (PanelWidgets &pw : ctx->panels) {
    const StopSnapshot *stop = findStopSnapshot(snap, pw.stop_key);
    bool have_data = stop != nullptr && stop->ok && !stop->arrivals.empty();

    // DESIGN.md SS6 alt_of: an alternative panel appears only while its primary stop has nothing
    // within alt_after_min (or no data), and disappears again when a bus is close.
    if (!pw.alt_of.empty()) {
      const StopSnapshot *primary = findStopSnapshot(snap, pw.alt_of);
      bool primary_far = true;
      if (primary != nullptr && primary->ok) {
        for (const Arrival &a : primary->arrivals) {
          transit::Epoch eff = a.effective();
          if (eff > 0 && eff - (transit::Epoch)now < (transit::Epoch)pw.alt_after_min * 60) {
            primary_far = false;
            break;
          }
        }
      }
      if (primary_far) {
        lv_obj_remove_flag(pw.panel, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(pw.panel, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
    }
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

    std::string note = (have_data && cfg.weather.enabled && cfg.weather.per_stop)
                           ? stopWeatherNote(pw.stop_key, stop->arrivals.front().effective())
                           : std::string();
    if (note.empty()) {
      lv_obj_add_flag(pw.weather_note, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_label_set_text(pw.weather_note, note.c_str());
      lv_obj_remove_flag(pw.weather_note, LV_OBJ_FLAG_HIDDEN);
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

      const std::string &cmode = cfg.device.crowding;
      bool want_icons = (cmode == "icons" || cmode == "both");
      bool want_words = (cmode == "words" || cmode == "both");
      std::string icons = want_icons ? crowdingIcons(a.seats, cfg.device.crowding_icons) : std::string();
      if (icons.empty()) {
        lv_obj_add_flag(rw.crowd_icons, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_label_set_text(rw.crowd_icons, icons.c_str());
        lv_obj_remove_flag(rw.crowd_icons, LV_OBJ_FLAG_HIDDEN);
      }
      std::string crowd = want_words ? crowdingText(a.seats) : std::string();
      if (crowd.empty()) {
        lv_obj_add_flag(rw.crowding, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_label_set_text(rw.crowding, crowd.c_str());
        lv_obj_remove_flag(rw.crowding, LV_OBJ_FLAG_HIDDEN);
      }

      transit::Epoch eff = a.effective();
      transit::Epoch eta_s = eff > 0 ? (eff - (transit::Epoch)now) : 0;
      lv_label_set_text(rw.minutes, etaLabel(eta_s, eff).c_str());
      // DESIGN.md SS6 due.screen: the minutes blink while this bus is within due.minutes.
      bool blink = cfg.due.screen && arrivalIsDue(cfg, a, (transit::Epoch)now) && ctx->blink_phase;
      lv_obj_set_style_text_color(rw.minutes, blink ? colorLate() : colorText(), 0);

      Badge badge = badgeFor(a);
      lv_label_set_text(rw.status_badge, badge.text.c_str());
      lv_obj_set_style_text_color(rw.status_badge, badge.color, 0);
    }
  }

  BikeView bikes = getBikes();
  if (!bikes.enabled || bikes.stations.empty()) {
    lv_obj_add_flag(ctx->bike_box, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(ctx->bike_box, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < ctx->bike_labels.size(); ++i) {
      if (i >= bikes.stations.size()) {
        lv_obj_add_flag(ctx->bike_labels[i], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      const indego::Station &st = bikes.stations[i];
      if (st.bikes < 0) {
        lv_label_set_text_fmt(ctx->bike_labels[i], "Indego %s: no data", st.name.c_str());
      } else if (!st.active) {
        lv_label_set_text_fmt(ctx->bike_labels[i], "Indego %s: offline", st.name.c_str());
      } else {
        lv_label_set_text_fmt(ctx->bike_labels[i], "Indego %s: %d bikes (%d e), %d docks", st.name.c_str(), st.bikes,
                              st.ebikes < 0 ? 0 : st.ebikes, st.docks);
      }
      lv_obj_remove_flag(ctx->bike_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // cfg.alerts off clears the ticker now rather than after the next poll drops the cached alerts.
  std::string ticker = cfg.alerts ? tickerText(snap.alerts, ctx->ticker_lines > 1, ctx->ticker_show) : std::string();
  if (ticker.empty()) {
    if (!ctx->ticker_text.empty()) {
      lv_anim_delete(ctx->ticker_label, nullptr);
      lv_label_set_text(ctx->ticker_label, "");
      ctx->ticker_text.clear();
    }
    lv_obj_add_flag(ctx->ticker_box, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(ctx->ticker_box, LV_OBJ_FLAG_HIDDEN);
    if (ticker != ctx->ticker_text) {
      ctx->ticker_text = ticker;
      lv_label_set_text(ctx->ticker_label, ticker.c_str());
      restartTicker(ctx);
    }
  }
}

}  // namespace transit_app::ui
