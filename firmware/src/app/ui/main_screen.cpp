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
#include "../../icons/weather_icons.h"
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
  lv_obj_t *weather_icon;   // lv_image of the colour condition icon (src/icons), hidden with weather_label
  lv_obj_t *weather_label;  // temperature ("69°"), or the words when no glyph fits
  lv_obj_t *wifi_bars;      // ui_common.h makeWifiBars(): four bars, no glyph, no number
  int wifi_bars_shown = -1;  // last count drawn, so the four bars are only restyled on a change
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
  // Indego section (DESIGN.md SS4.9/SS8): header row, then one row per configured station.
  lv_obj_t *bike_box;
  lv_obj_t *bike_age;  // "12 min old" in amber when the feed is stale, hidden otherwise
  struct BikeRow {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *counts;  // bikeCounts() with recolor; fontIcons() or fontSmall() per bike.style
  };
  std::vector<BikeRow> bike_rows;
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

// makeBox()/makeLabel() and the header/panel builders live in ui_common.cpp now that the stats
// and device pages are built from the same pieces (DESIGN.md SS8).

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
  // The screen's CAPACITY per panel (DESIGN.md SS8: 320x240 gets 2, 480x320 gets 3, large-text
  // always 2). How many rows a given stop actually gets is its own `show` clamped to this - see
  // the panel loop below (F31).
  int row_capacity = rowsPerStop(h, large);
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

  // ---- Header (10% height; ui_common.h makeHeader, shared with the stats and device pages) ----
  int32_t header_h = headerHeight(h);
  lv_obj_t *header = makeHeader(screen, h);
  ctx->header = header;

  ctx->device_label = makeLabel(header, fontSmall(h), colorText());
  lv_label_set_text(ctx->device_label, cfg.device.name.c_str());

  ctx->clock_label = makeLabel(header, fontSmall(h), colorText());
  lv_label_set_text(ctx->clock_label, "--:--");

  ctx->weather_icon = lv_image_create(header);
  lv_obj_add_flag(ctx->weather_icon, LV_OBJ_FLAG_HIDDEN);  // shown once a forecast picks an icon
  ctx->weather_label = makeLabel(header, fontSmall(h), colorText());
  lv_label_set_text(ctx->weather_label, "");

  lv_obj_t *right_group = makeBox(header);
  lv_obj_set_style_bg_opa(right_group, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(right_group, 0, 0);
  lv_obj_set_size(right_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(right_group, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right_group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(right_group, 8, 0);

  ctx->wifi_bars = makeWifiBars(right_group, header_h);

  ctx->updated_label = makeLabel(right_group, fontSmall(h), colorSubtext());
  lv_label_set_text(ctx->updated_label, "updated -- ago");

  // DESIGN.md SS8: config.device.header picks what the (narrow) header shows; hidden flex items
  // take no space, so the remaining ones spread out.
  const HeaderConfig &hc = cfg.device.header;
  if (!hc.name) lv_obj_add_flag(ctx->device_label, LV_OBJ_FLAG_HIDDEN);
  if (!hc.clock) lv_obj_add_flag(ctx->clock_label, LV_OBJ_FLAG_HIDDEN);
  if (!hc.weather || !cfg.weather.enabled) {
    lv_obj_add_flag(ctx->weather_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ctx->weather_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (!hc.wifi) lv_obj_add_flag(ctx->wifi_bars, LV_OBJ_FLAG_HIDDEN);
  if (!hc.updated) lv_obj_add_flag(ctx->updated_label, LV_OBJ_FLAG_HIDDEN);

  // ---- Stop panels (ui_common.h makePanelsArea/makePanel: the stats page uses the same) ----
  lv_obj_t *panels_area = makePanelsArea(screen);

  for (const StopConfig &s : visibleStops(cfg, time(nullptr))) {
    PanelWidgets pw;
    pw.stop_key = s.key;
    pw.route = s.route;
    pw.alt_of = s.alt_of;
    pw.alt_after_min = s.alt_after_min;

    lv_obj_t *panel = makePanel(panels_area);
    pw.panel = panel;
    if (!s.alt_of.empty()) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);  // until the primary runs late

    pw.title = makeLabel(panel, fontBody(h), colorText());
    lv_label_set_text(pw.title, panelTitle(s).c_str());
    // Both sizes fixed: LONG_DOT only ellipsizes a label whose height is not content-sized -
    // with a content height a long title wrapped onto a second line instead (DESIGN.md SS8 says
    // ellipsized), which on a 240-tall board pushed the last arrival row out of the panel.
    lv_obj_set_size(pw.title, lv_pct(100), lv_font_get_line_height(fontBody(h)));
    lv_label_set_long_mode(pw.title, LV_LABEL_LONG_DOT);

    pw.weather_note = makeLabel(panel, fontSmall(h), colorEarly());
    lv_obj_set_width(pw.weather_note, lv_pct(100));
    lv_label_set_long_mode(pw.weather_note, LV_LABEL_LONG_DOT);
    lv_label_set_text(pw.weather_note, "");
    lv_obj_add_flag(pw.weather_note, LV_OBJ_FLAG_HIDDEN);

    pw.no_data_label = makeLabel(panel, fontSmall(h), colorSubtext());
    lv_label_set_text(pw.no_data_label, "no data yet");
    lv_obj_add_flag(pw.no_data_label, LV_OBJ_FLAG_HIDDEN);

    // DESIGN.md SS6/SS8: each stop shows `show` arrival rows (1..4). F31 - this loop used to run to
    // the screen's capacity for every panel, so the per-stop setting was accepted by the config
    // schema, echoed back by GET /api/config, and then ignored by the only thing that could act on
    // it: a stop asking for 1 row still got 3. Capacity is still the ceiling - four 48 px rows do
    // not fit a 240 px panel however politely they are requested.
    int panel_rows = std::min<int>(row_capacity, std::max<int>(1, (int)s.show));
    for (int r = 0; r < panel_rows; ++r) {
      lv_obj_t *row = makeBox(panel);
      lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(row, 2, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 6, 0);

      RowWidgets rw;
      rw.route_badge = makeRouteBadge(row, fontSmall(h), s.route);

      rw.destination = makeLabel(row, fontBody(h), colorText());
      lv_obj_set_flex_grow(rw.destination, 1);
      // Fixed to one line for the same reason as the title: with crowding words and icons on the
      // row, "20th-Johnston" wrapped onto two lines and the row grew instead of ellipsizing.
      lv_obj_set_height(rw.destination, lv_font_get_line_height(fontBody(h)));
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

  // ---- Indego section (only shown when bike_service has stations) ----
  // A panel like the stop panels: "[bicycle] Indego" header with a stale-age note on the right,
  // then one row per station: name (ellipsized) and the counts meter right-aligned.
  ctx->bike_box = makeBox(screen);
  lv_obj_set_size(ctx->bike_box, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(ctx->bike_box, colorPanelBg(), 0);
  lv_obj_set_style_pad_all(ctx->bike_box, 4, 0);
  lv_obj_set_style_pad_row(ctx->bike_box, 2, 0);
  lv_obj_set_flex_flow(ctx->bike_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(ctx->bike_box, LV_OBJ_FLAG_HIDDEN);
  auto makeBikeRow = [&](lv_obj_t *parent) {
    lv_obj_t *row = makeBox(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
  };
  {
    lv_obj_t *head = makeBikeRow(ctx->bike_box);
    lv_obj_t *icon = makeLabel(head, fontIcons(), colorText());
    lv_label_set_text(icon, "\xEF\x88\x86");  // U+F206 bicycle
    lv_obj_t *title = makeLabel(head, fontBody(h), colorText());
    lv_label_set_text(title, "Indego");
    ctx->bike_age = makeLabel(head, fontSmall(h), colorSkipped());
    lv_obj_set_flex_grow(ctx->bike_age, 1);
    lv_obj_set_style_text_align(ctx->bike_age, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(ctx->bike_age, "");
    lv_obj_add_flag(ctx->bike_age, LV_OBJ_FLAG_HIDDEN);
  }
  for (size_t i = 0; i < kMaxBikeStations; ++i) {
    MainScreenCtx::BikeRow br;
    br.row = makeBikeRow(ctx->bike_box);
    br.name = makeLabel(br.row, fontSmall(h), colorText());
    lv_obj_set_flex_grow(br.name, 1);
    lv_label_set_long_mode(br.name, LV_LABEL_LONG_DOT);
    lv_label_set_text(br.name, "");
    br.counts = makeLabel(br.row, fontIcons(), colorText());
    lv_label_set_recolor(br.counts, true);
    lv_obj_set_style_text_letter_space(br.counts, 1, 0);
    lv_label_set_text(br.counts, "");
    lv_obj_add_flag(br.row, LV_OBJ_FLAG_HIDDEN);
    ctx->bike_rows.push_back(br);
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

void mainScreenDebug(lv_obj_t *screen, std::vector<std::string> &hidden_panels, std::string &ticker_text,
                     std::string &rows_debug) {
  auto *ctx = static_cast<MainScreenCtx *>(lv_obj_get_user_data(screen));
  hidden_panels.clear();
  ticker_text.clear();
  rows_debug.clear();
  if (ctx == nullptr) return;
  for (const PanelWidgets &pw : ctx->panels) {
    if (lv_obj_has_flag(pw.panel, LV_OBJ_FLAG_HIDDEN)) {
      hidden_panels.push_back(pw.stop_key);
      continue;
    }
    for (const RowWidgets &rw : pw.rows) {
      lv_obj_t *row = lv_obj_get_parent(rw.minutes);
      if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) continue;
      char buf[160];
      snprintf(buf, sizeof buf, "%s|%s|%s|icons h=%d len=%u w=%d x=%d|word h=%d '%s'|row w=%d\n", pw.stop_key.c_str(),
               lv_label_get_text(rw.destination), lv_label_get_text(rw.minutes),
               lv_obj_has_flag(rw.crowd_icons, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
               (unsigned)strlen(lv_label_get_text(rw.crowd_icons)), (int)lv_obj_get_width(rw.crowd_icons),
               (int)lv_obj_get_x(rw.crowd_icons), lv_obj_has_flag(rw.crowding, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
               lv_label_get_text(rw.crowding), (int)lv_obj_get_width(row));
      rows_debug += buf;
    }
  }
  if (!lv_obj_has_flag(ctx->bike_box, LV_OBJ_FLAG_HIDDEN)) {
    char buf[120];
    snprintf(buf, sizeof buf, "bike|age h=%d '%s'\n", lv_obj_has_flag(ctx->bike_age, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
             lv_label_get_text(ctx->bike_age));
    rows_debug += buf;
    for (const MainScreenCtx::BikeRow &br : ctx->bike_rows) {
      if (lv_obj_has_flag(br.row, LV_OBJ_FLAG_HIDDEN)) continue;
      snprintf(buf, sizeof buf, "bike|%s|counts len=%u w=%d h=%d|name w=%d\n", lv_label_get_text(br.name),
               (unsigned)strlen(lv_label_get_text(br.counts)), (int)lv_obj_get_width(br.counts),
               (int)lv_obj_get_height(br.counts), (int)lv_obj_get_width(br.name));
      rows_debug += buf;
    }
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
    // Colour icon + temperature ("[sun] 69°"); the words only while there is no forecast.
    static const lv_image_dsc_t *const kIcons[] = {nullptr, &wx_sun, &wx_moon, &wx_cloud_sun, &wx_cloud_moon, &wx_cloud,
                                                   &wx_rain, &wx_showers, &wx_snow, &wx_fog, &wx_storm};
    const lv_image_dsc_t *icon = kIcons[(int)headerWeatherIcon()];
    if (icon == nullptr) {
      lv_obj_add_flag(ctx->weather_icon, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(ctx->weather_label, headerWeatherText().c_str());
    } else {
      if (lv_image_get_src(ctx->weather_icon) != icon) lv_image_set_src(ctx->weather_icon, icon);
      lv_obj_remove_flag(ctx->weather_icon, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(ctx->weather_label, headerWeatherTemp().c_str());
    }
  }

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
  bool stale_changed = stale != ctx->header_stale;
  if (stale_changed) {
    ctx->header_stale = stale;
    lv_obj_set_style_bg_color(ctx->header, stale ? colorStale() : colorPanelBg(), 0);
    lv_color_t fg = stale ? colorOnStale() : colorText();
    lv_color_t sub = stale ? colorOnStale() : colorSubtext();
    lv_obj_set_style_text_color(ctx->device_label, fg, 0);
    lv_obj_set_style_text_color(ctx->clock_label, fg, 0);
    lv_obj_set_style_text_color(ctx->weather_label, fg, 0);
    lv_obj_set_style_text_color(ctx->updated_label, sub, 0);
  }

  // Wi-Fi bars (ui_common.h): lit bars in the header text colour, the rest dim; on the amber
  // stale header they take its dark-on-amber colours like every other header item.
  int bars = wifiBarCount(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100, WiFi.status() == WL_CONNECTED);
  if (bars != ctx->wifi_bars_shown || stale_changed) {
    ctx->wifi_bars_shown = bars;
    setWifiBars(ctx->wifi_bars, bars, stale ? colorOnStale() : colorText(), stale ? colorStale() : colorPanelBg());
  }

  ctx->blink_phase = !ctx->blink_phase;
  for (PanelWidgets &pw : ctx->panels) {
    const StopSnapshot *stop = findStopSnapshot(snap, pw.stop_key);
    // F31: rows are shown whenever the stop HAS rows, not only when its whole fetch succeeded.
    // StopSnapshot::ok and ::health answer different questions (model.h): a stop whose live feed
    // arrived truncated is ok=false and Health::ScheduleOnly with a perfectly good set of
    // scheduled times on it. Testing `ok` alone blanked those - the user lost the schedule they
    // could have used because the realtime half failed. Health::Unavailable is the one case with
    // nothing trustworthy behind it, so that one shows the reason and no rows.
    bool has_rows = stop != nullptr && !stop->arrivals.empty();
    bool have_data = has_rows && stop->health != transit::Health::Unavailable;

    // DESIGN.md SS6 alt_of: an alternative panel appears only while its primary stop has nothing
    // within alt_after_min (or no data), and disappears again when a bus is close.
    if (!pw.alt_of.empty()) {
      const StopSnapshot *primary = findStopSnapshot(snap, pw.alt_of);
      bool primary_far = true;
      // Same rule as the panel above (F31): schedule-only rows are rows. Testing `ok` here made a
      // primary whose live feed failed look like a primary with nothing coming, which popped the
      // alternative up over a stop that had a bus due in four minutes on the timetable.
      if (primary != nullptr && primary->health != transit::Health::Unavailable) {
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
    // DESIGN.md SS8: "Stale data: ... 'stale 4 min'; no data: panel shows the reason". F31 - the
    // caption now carries the stop's HEALTH as well as its emptiness, because they are different
    // facts and the user needs both: three schedule rows with no caption look exactly like three
    // live rows, and a panel that has quietly stopped updating looks exactly like a quiet stop.
    // Same styling as the old reason line; it is the same question ("why does this look like
    // this?"), asked whether or not there are rows underneath.
    std::string caption;
    if (stop == nullptr) {
      caption = snap.generated == 0 ? "no data yet" : "no data";
    } else {
      switch (stop->health) {
        case transit::Health::Unavailable:
          caption = stop->error.empty() ? "unavailable" : stop->error;
          break;
        case transit::Health::Stale: {
          // Measured from source_ts - when SEPTA produced the data - not from our own fetch time.
          // A feed we re-fetch every 30 seconds is always "just fetched"; what went stale is what
          // the agency last published (model.h). source_ts == 0 means the feed published no
          // timestamp at all, and an age we do not know must not be invented.
          int64_t age_s = stop->source_ts > 0 ? ((int64_t)now - (int64_t)stop->source_ts) : -1;
          if (age_s >= 0) {
            char buf[32];
            snprintf(buf, sizeof buf, "stale %ld min", (long)(age_s / 60));
            caption = buf;
          } else {
            caption = stop->error.empty() ? "stale" : stop->error;
          }
          break;
        }
        case transit::Health::ScheduleOnly:
          // Not an error: a subway stop has no realtime source at all (DESIGN.md SS4.6), and a bus
          // stop lands here when the live feed failed but the schedule did not. Either way the
          // times below are timetable, not tracking, and the badge column's grey "sched" says that
          // per row while this says it once for the panel.
          caption = stop->error.empty() ? "schedule only" : ("schedule only: " + stop->error);
          break;
        default:
          if (!stop->ok) caption = stop->error.empty() ? "unavailable" : stop->error;
          break;
      }
      if (caption.empty() && !has_rows) caption = "no arrivals";
    }
    if (caption.empty()) {
      lv_obj_add_flag(pw.no_data_label, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_label_set_text(pw.no_data_label, caption.c_str());
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

      // Arrival::effective() is predicted-or-scheduled, which is what makes a Status::Skipped row
      // with predicted == 0 render from its SCHEDULED time (F31): SEPTA publishes a skipped stop
      // by dropping the prediction, not by moving it, so the timetable entry is all there is - and
      // the orange "skip" badge from badgeFor() is what turns that time into "this bus is coming
      // past at 4:12 but not stopping here". A row with neither time is a row we cannot place;
      // "--" says so, where the old code passed 0 through and drew "Now".
      transit::Epoch eff = a.effective();
      if (eff <= 0) {
        lv_label_set_text(rw.minutes, "--");
      } else {
        lv_label_set_text(rw.minutes, etaLabel(eff - (transit::Epoch)now, eff).c_str());
      }
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
    const bool icons = cfg.bike.style != "words";
    if (bikes.fetched_epoch == 0) {
      lv_label_set_text(ctx->bike_age, "no data yet");
      lv_obj_remove_flag(ctx->bike_age, LV_OBJ_FLAG_HIDDEN);
    } else if ((uint32_t)now > bikes.fetched_epoch + 600) {
      lv_label_set_text_fmt(ctx->bike_age, "%u min old", (unsigned)(((uint32_t)now - bikes.fetched_epoch) / 60));
      lv_obj_remove_flag(ctx->bike_age, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ctx->bike_age, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < ctx->bike_rows.size(); ++i) {
      MainScreenCtx::BikeRow &br = ctx->bike_rows[i];
      if (i >= bikes.stations.size()) {
        lv_obj_add_flag(br.row, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      const indego::Station &st = bikes.stations[i];
      lv_label_set_text(br.name, st.name.c_str());
      if (st.bikes < 0 || !st.active) {
        // The icon font has no letters: switch this row's meter to the text font for the note.
        lv_obj_set_style_text_font(br.counts, fontSmall(0), 0);
        lv_obj_set_style_text_color(br.counts, colorSubtext(), 0);
        lv_label_set_text(br.counts, st.bikes < 0 ? "no data" : "offline");
      } else {
        int ebikes = st.ebikes < 0 ? 0 : st.ebikes;
        int classic = st.classic >= 0 ? st.classic : st.bikes - ebikes;
        lv_obj_set_style_text_font(br.counts, icons ? fontIcons() : fontSmall(0), 0);
        lv_obj_set_style_text_color(br.counts, colorText(), 0);
        std::string text = bikeCounts(classic, ebikes, st.docks, icons);
        lv_label_set_text(br.counts, text.c_str());
      }
      lv_obj_remove_flag(br.row, LV_OBJ_FLAG_HIDDEN);
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
