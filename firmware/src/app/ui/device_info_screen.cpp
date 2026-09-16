// Device page in the main page's visual language (DESIGN.md SS8): the header strip with the
// firmware version, a "Network" panel (Wi-Fi bars, SSID, the mDNS URL - how the owner reaches
// the web app - and the IP), a panel named after the device (the web PIN in the big font, SD
// status and write health, heap, uptime), a "Data sources" panel when the height allows it (one
// line per feed the config has on - SEPTA, weather, Indego, alerts - with a status word in the
// arrival colours and how long ago it was fetched; on a board where it does not fit, the SEPTA
// line stays in the Network panel), and the hold-5-seconds Wi-Fi reset as a bordered button
// along the bottom.
#include "device_info_screen.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <ctime>
#include <string>

#include "../auth.h"
#include "../bike_service.h"
#include "../net_poller.h"
#include "../sd_logger.h"
#include "../weather_service.h"
#include "ui_common.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

namespace transit_app::ui {

namespace {

constexpr uint32_t kWifiResetHoldMs = 5000;
const char *const kResetIdle = "reset Wi-Fi: hold 5 s";
const char *const kResetHeld = "keep holding to confirm";

struct DeviceInfoCtx {
  lv_obj_t *wifi_bars;
  int wifi_bars_shown = -1;
  lv_obj_t *ssid_label;
  lv_obj_t *url_label;
  lv_obj_t *ip_label;
  lv_obj_t *rssi_label;
  lv_obj_t *septa_label;               // in the Data sources panel, or in Network when that does not fit
  bool septa_inline = false;           // ...in which case the line carries its own "SEPTA" caption
  lv_obj_t *weather_label = nullptr;   // Data sources rows; nullptr when the panel is absent or the feed is off
  lv_obj_t *bike_label = nullptr;
  lv_obj_t *alerts_label = nullptr;
  uint16_t poll_seconds = 30;          // device.poll_seconds: "stale" for SEPTA is measured against it
  lv_obj_t *uptime_label;
  lv_obj_t *pin_label;
  lv_obj_t *sd_label;
  lv_obj_t *heap_label;
  lv_obj_t *reset_label;
  std::string mdns_host;
  uint32_t press_start_ms = 0;
};

// Unchanged from the first version of this page: PRESSED starts the clock, RELEASED after 5 s
// erases the credentials and reboots, PRESS_LOST (finger slid off) just cancels.
void resetTargetEventCb(lv_event_t *e) {
  auto *ctx = static_cast<DeviceInfoCtx *>(lv_event_get_user_data(e));
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    ctx->press_start_ms = millis();
    lv_label_set_text(ctx->reset_label, kResetHeld);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    uint32_t held = millis() - ctx->press_start_ms;
    lv_label_set_text(ctx->reset_label, kResetIdle);
    if (code == LV_EVENT_RELEASED && ctx->press_start_ms != 0 && held >= kWifiResetHoldMs) {
      log_w("device_info_screen: Wi-Fi reset gesture confirmed, erasing credentials and rebooting");
      // Same as web_server.cpp's /api/wifi/reset handler: erase the ESP-IDF's persisted STA
      // credentials so main.cpp's wifi_portal::connectWifiOrPortal() opens the setup AP again.
      WiFi.mode(WIFI_STA);
      WiFi.disconnect(true, true);
      delay(200);
      ESP.restart();
    }
    ctx->press_start_ms = 0;
  }
}

// A content-height panel: as tall as its lines rather than sharing the height like stop panels.
// A mostly-empty white card on a 480 px screen looks like a fault, and there are no rows to
// fill it with.
lv_obj_t *infoPanel(lv_obj_t *area) {
  lv_obj_t *panel = makePanel(area);
  lv_obj_set_flex_grow(panel, 0);
  lv_obj_set_height(panel, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_row(panel, 2, 0);
  return panel;
}

// A panel's title row: the name on the left, whatever the caller adds after it on the right.
lv_obj_t *titleRow(lv_obj_t *panel, int32_t h, const char *name) {
  lv_obj_t *row = makeRow(panel, 6);
  lv_obj_t *t = makeLabel(row, fontBody(h), colorText());
  lv_label_set_text(t, name);
  lv_obj_set_flex_grow(t, 1);
  lv_obj_set_height(t, lv_font_get_line_height(fontBody(h)));
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);  // a long device name ellipsizes, never wraps
  return row;
}

// "caption  value" on one line, the caption in the secondary colour via inline recolor: one
// label per line keeps the page at ~30 objects in LVGL's 36 KB pool.
lv_obj_t *captionedLine(lv_obj_t *panel, int32_t h) {
  lv_obj_t *l = makeLabel(panel, fontSmall(h), colorText());
  lv_label_set_recolor(l, true);
  lv_obj_set_size(l, lv_pct(100), lv_font_get_line_height(fontSmall(h)));
  // CLIP, not DOT: LONG_DOT counts the recolor command as visible text when placing the dots
  // (stats_screen.cpp has the same note), so a line that fits would lose its last word.
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_label_set_text(l, "");
  return l;
}

// One Data sources row: a fixed-width caption so the status words line up in a column, then a
// recolor-enabled value that clips. `half` makes the row half the panel wide for the two-column
// form on a 480-wide board.
lv_obj_t *sourceRow(lv_obj_t *parent, int32_t h, const char *caption, bool half) {
  lv_obj_t *row = makeRow(parent, 6);
  if (half) lv_obj_set_width(row, lv_pct(50));
  lv_obj_t *cap = makeLabel(row, fontSmall(h), colorSubtext());
  lv_obj_set_width(cap, 64);
  lv_label_set_text(cap, caption);
  lv_obj_t *val = makeLabel(row, fontSmall(h), colorText());
  lv_label_set_recolor(val, true);
  lv_obj_set_flex_grow(val, 1);
  lv_obj_set_height(val, lv_font_get_line_height(fontSmall(h)));
  lv_label_set_long_mode(val, LV_LABEL_LONG_CLIP);
  lv_label_set_text(val, "");
  return val;
}

void uptimeText(uint32_t s, char *out, size_t n) {
  unsigned d = s / 86400, hr = (s / 3600) % 24, m = (s / 60) % 60;
  if (d > 0) {
    snprintf(out, n, "up %ud %uh", d, hr);
  } else if (hr > 0) {
    snprintf(out, n, "up %uh %um", hr, m);
  } else {
    snprintf(out, n, "up %um", m);
  }
}

// "12 s ago", "4 min ago", "3 h ago".
void agoText(uint32_t s, char *out, size_t n) {
  if (s < 60) {
    snprintf(out, n, "%u s ago", (unsigned)s);
  } else if (s < 3600) {
    snprintf(out, n, "%u min ago", (unsigned)(s / 60));
  } else {
    snprintf(out, n, "%u h ago", (unsigned)(s / 3600));
  }
}

// "ok, 12 s ago" / "stale, 2 h ago" / "failed, 4 min ago: connect failed" / "no data yet": the
// status word in the arrival colours (green ok, amber stale, red failed, grey nothing yet), the
// age when there is one (age_s < 0: none), the feed's own reason when it has one. A Data sources
// row has its caption in a column of its own; the SEPTA line that falls back into the Network
// panel carries "SEPTA  " inline (`caption`), like the SD and heap lines.
void setSource(lv_obj_t *label, const char *caption, const char *word, lv_color_t color, int32_t age_s,
               const char *detail) {
  char age[16] = "";
  if (age_s >= 0) agoText((uint32_t)age_s, age, sizeof age);
  char buf[112];
  int n = caption ? snprintf(buf, sizeof buf, "#%06x %s#  ", (unsigned)colorHex(colorSubtext()), caption) : 0;
  snprintf(buf + n, sizeof buf - (size_t)n, "#%06x %s#%s%s%s%s", (unsigned)colorHex(color), word, age[0] ? ", " : "",
           age, detail && detail[0] ? ": " : "", detail ? detail : "");
  lv_label_set_text(label, buf);
}

}  // namespace

lv_obj_t *createDeviceInfoScreen(const Config &cfg) {
  int32_t w, h;
  screenSize(w, h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);  // tap anywhere (except the reset target) cycles pages

  auto *ctx = new DeviceInfoCtx();
  ctx->mdns_host = cfg.device.name + ".local";
  ctx->poll_seconds = cfg.device.poll_seconds;

  // ---- Does the Data sources panel fit? Decided from the heights, like the stats page's layouts ----
  // Network (three lines once SEPTA moves out), the device panel (its PIN line in the big font on
  // a 320-tall board), the reset button, four 4 px gaps, and the panel itself: title plus one
  // line per feed the config has on, two feeds per line on a 480-wide board. 240-tall boards never
  // get it (the coordinator's brief: their layout stays as it is); 480x320 gets the two-column
  // form with a few px to spare; 320x480 has room for everything.
  int32_t lh = lv_font_get_line_height(fontSmall(h));
  bool big_pin = h >= 320 && auth::pin().size() <= 8;
  int32_t lh_pin = big_pin ? lv_font_get_line_height(fontBig(h)) : lh;
  int sources = 1 + (cfg.weather.enabled ? 1 : 0) + (cfg.bike.enabled ? 1 : 0) + (cfg.alerts ? 1 : 0);
  bool two_cols = w >= 400;
  int source_lines = two_cols ? (sources + 1) / 2 : sources;
  int32_t reset_pad = h >= 320 ? 8 : 6;
  int32_t net_h = 12 + 3 * lh + 2 * 2;
  int32_t dev_h = 12 + lh + lh_pin + 2 * lh + 3 * 2;
  int32_t reset_h = lh + 2 * reset_pad + 2;
  int32_t card_h = 12 + lh + source_lines * (lh + 2);
  int32_t area_h = h - headerHeight(h) - 8;
  bool sources_card = h >= 320 && area_h - net_h - dev_h - reset_h - 4 * 4 >= card_h;

  // ---- Header ----
  lv_obj_t *header = makeHeader(screen, h);
  lv_obj_t *title = makeLabel(header, fontBody(h), colorText());
  lv_label_set_recolor(title, true);
  lv_label_set_text_fmt(title, "Device info  #%06x v" FIRMWARE_VERSION "#", (unsigned)colorHex(colorSubtext()));
  lv_obj_t *hint = makeLabel(header, fontSmall(h), colorSubtext());
  lv_label_set_text(hint, "tap for arrivals");
  if (w < 300) lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);  // 240 wide: the title alone fills the strip

  lv_obj_t *area = makePanelsArea(screen);

  // ---- Network ----
  lv_obj_t *net = infoPanel(area);
  {
    lv_obj_t *row = titleRow(net, h, "Network");
    ctx->wifi_bars = makeWifiBars(row, headerHeight(h));
    ctx->ssid_label = makeLabel(row, fontSmall(h), colorText());
    lv_label_set_long_mode(ctx->ssid_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(ctx->ssid_label, lv_pct(55), 0);  // a long SSID ellipsizes, never pushes the bars off
    lv_label_set_text(ctx->ssid_label, "");
  }
  // The address of the web app, in link blue and the body font: this line is how the owner
  // reaches every setting, so it is the one thing on the page that must be findable at a glance.
  ctx->url_label = makeLabel(net, fontBody(h), colorEarly());
  lv_obj_set_width(ctx->url_label, lv_pct(100));
  lv_label_set_long_mode(ctx->url_label, LV_LABEL_LONG_DOT);
  lv_label_set_text_fmt(ctx->url_label, "http://%s/", ctx->mdns_host.c_str());
  {
    lv_obj_t *row = makeRow(net, 6);
    ctx->ip_label = makeLabel(row, fontSmall(h), colorText());
    lv_obj_set_flex_grow(ctx->ip_label, 1);
    lv_label_set_text(ctx->ip_label, "");
    ctx->rssi_label = makeLabel(row, fontSmall(h), colorSubtext());  // dBm is fine on a diagnostics page
    lv_label_set_text(ctx->rssi_label, "");
  }
  // Whether the data behind the arrivals page is actually arriving: the last poll's outcome and
  // age, with net_poller's own error text when it failed. The arrivals header only says "stale".
  // Lives in the Data sources panel below when that fits, here otherwise.
  if (!sources_card) {
    ctx->septa_label = captionedLine(net, h);
    ctx->septa_inline = true;
  }

  // ---- This device, by name ----
  lv_obj_t *dev = infoPanel(area);
  {
    lv_obj_t *row = titleRow(dev, h, cfg.device.name.empty() ? "Device" : cfg.device.name.c_str());
    ctx->uptime_label = makeLabel(row, fontSmall(h), colorSubtext());
    lv_label_set_text(ctx->uptime_label, "");
  }
  {
    // One of the two ways the owner learns the admin PIN (auth.h); the other is the serial
    // console at boot. Both need physical possession of the display, which is the point: there
    // is no network path to it. Printed plainly rather than hidden behind a gesture because
    // someone who can read this screen can already unplug the device, press-and-hold to wipe
    // its Wi-Fi, or walk off with it - the PIN is not what is protecting it from them.
    //
    // In the big minutes font on the 320-tall boards: it is the number the owner reads off the
    // panel to type into a phone, so it gets the arrivals page's most legible size. A custom PIN
    // can be 32 characters (auth.h) and would wrap across three lines at that size, so past
    // eight characters it drops to the body font and wraps inside three quarters of the row -
    // wraps rather than ellipsizes, because a PIN the owner cannot read off the screen is a PIN
    // they have lost.
    lv_obj_t *row = makeRow(dev, 8);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    ctx->pin_label = makeLabel(row, big_pin ? fontBig(h) : fontBody(h), colorText());
    lv_label_set_long_mode(ctx->pin_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(ctx->pin_label, lv_pct(75), 0);
    lv_label_set_text(ctx->pin_label, "");
    lv_obj_t *cap = makeLabel(row, fontSmall(h), colorSubtext());
    if (big_pin) lv_obj_set_style_pad_bottom(cap, (lh_pin - lh) / 6, 0);
    lv_label_set_text(cap, "web PIN");
  }
  ctx->sd_label = captionedLine(dev, h);
  ctx->heap_label = captionedLine(dev, h);

  // ---- Data sources: one line per feed the config has on ----
  if (sources_card) {
    lv_obj_t *card = infoPanel(area);
    titleRow(card, h, "Data sources");
    lv_obj_t *rows = card;
    if (two_cols) {
      rows = makeBox(card);
      lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
      lv_obj_set_size(rows, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_style_pad_row(rows, 2, 0);
      lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_ROW_WRAP);
    }
    ctx->septa_label = sourceRow(rows, h, "SEPTA", two_cols);
    if (cfg.weather.enabled) ctx->weather_label = sourceRow(rows, h, "Weather", two_cols);
    if (cfg.bike.enabled) ctx->bike_label = sourceRow(rows, h, "Indego", two_cols);
    if (cfg.alerts) ctx->alerts_label = sourceRow(rows, h, "Alerts", two_cols);
  }

  lv_obj_t *spacer = makeBox(area);  // takes whatever height the two panels leave
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_size(spacer, lv_pct(100), 0);
  lv_obj_set_flex_grow(spacer, 1);

  // ---- Wi-Fi reset: a bordered button along the bottom ----
  // The only clickable child on any page (ui_common.h makeBox leaves everything else
  // unclickable), so a press here goes to resetTargetEventCb and nowhere else.
  lv_obj_t *reset_target = lv_obj_create(area);
  lv_obj_set_size(reset_target, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(reset_target, colorPanelBg(), 0);
  lv_obj_set_style_bg_opa(reset_target, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(reset_target, 1, 0);
  lv_obj_set_style_border_color(reset_target, colorSubtext(), 0);
  lv_obj_set_style_border_color(reset_target, colorLate(), LV_STATE_PRESSED);  // LVGL applies it while pressed
  lv_obj_set_style_pad_all(reset_target, reset_pad, 0);
  lv_obj_remove_flag(reset_target, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(reset_target, LV_OBJ_FLAG_CLICKABLE);
  ctx->reset_label = makeLabel(reset_target, fontSmall(h), colorSubtext());
  lv_obj_set_width(ctx->reset_label, lv_pct(100));
  lv_obj_set_style_text_align(ctx->reset_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(ctx->reset_label, kResetIdle);
  lv_obj_add_event_cb(reset_target, resetTargetEventCb, LV_EVENT_PRESSED, ctx);
  lv_obj_add_event_cb(reset_target, resetTargetEventCb, LV_EVENT_RELEASED, ctx);
  lv_obj_add_event_cb(reset_target, resetTargetEventCb, LV_EVENT_PRESS_LOST, ctx);

  lv_obj_set_user_data(screen, ctx);
  lv_obj_add_event_cb(screen, [](lv_event_t *e) {  // see main_screen.cpp: freed with the screen
    lv_obj_t *scr = static_cast<lv_obj_t *>(lv_event_get_target(e));
    delete static_cast<DeviceInfoCtx *>(lv_obj_get_user_data(scr));
    lv_obj_set_user_data(scr, nullptr);
  }, LV_EVENT_DELETE, nullptr);
  return screen;
}

void refreshDeviceInfoScreen(lv_obj_t *screen) {
  auto *ctx = static_cast<DeviceInfoCtx *>(lv_obj_get_user_data(screen));
  if (ctx == nullptr) {
    return;
  }

  bool connected = WiFi.status() == WL_CONNECTED;
  int rssi = connected ? WiFi.RSSI() : -100;
  int bars = wifiBarCount(rssi, connected);
  if (bars != ctx->wifi_bars_shown) {
    ctx->wifi_bars_shown = bars;
    setWifiBars(ctx->wifi_bars, bars, colorText(), colorPanelBg());
  }
  lv_label_set_text(ctx->ssid_label, connected ? WiFi.SSID().c_str() : "not connected");
  lv_label_set_text(ctx->ip_label, connected ? WiFi.localIP().toString().c_str() : "no IP address");
  if (connected) {
    lv_label_set_text_fmt(ctx->rssi_label, "%d dBm", rssi);
  } else {
    lv_label_set_text(ctx->rssi_label, "");
  }

  // Every line here is composed with snprintf and inline recolor commands (ui_common.h
  // colorHex), not std::string.
  char buf[96];
  unsigned sub = (unsigned)colorHex(colorSubtext());
  uint32_t now = (uint32_t)time(nullptr);
  bool clock_set = now > 1700000000u;  // an epoch before 2023 is the boot default, not a time

  // SEPTA: the last poll's outcome and age. "stale" when the last success is older than two poll
  // intervals plus 30 s (90 s at the default 30 s, the arrivals header's own threshold) - the
  // poller has stopped getting through even though nothing has reported a failure yet. The age
  // is left off until the clock has been set.
  {
    // tryGetPollStatus(), not getPollStatus(): this runs on the LVGL task on every tick the device
    // page is shown, and getPollStatus() waits up to a second on the poller's mutex. DESIGN.md SS5
    // says the display task must never block on the poller, and SS12.1 records the
    // vTaskPriorityDisinheritAfterTimeout assert that exactly this 1 s wait produced. The 50 ms cap
    // is getStopSummary()'s. `last` is function-static and touched only from this task: on a miss
    // the line redraws with the value it last read - one tick of staleness on a page that is
    // already showing an age in seconds - rather than blanking to "no poll yet".
    static PollStatus last;
    tryGetPollStatus(&last);
    const PollStatus &ps = last;
    int32_t age = ps.has_polled && clock_set && now >= ps.last_poll_epoch ? (int32_t)(now - ps.last_poll_epoch) : -1;
    const char *cap = ctx->septa_inline ? "SEPTA" : nullptr;
    if (!ps.has_polled) {
      setSource(ctx->septa_label, cap, "no poll yet", colorScheduled(), -1, nullptr);
    } else if (!ps.ok) {
      setSource(ctx->septa_label, cap, "failed", colorLate(), age, ps.last_error.c_str());
    } else if (age > 2 * (int32_t)ctx->poll_seconds + 30) {
      setSource(ctx->septa_label, cap, "stale", colorSkipped(), age, nullptr);
    } else {
      setSource(ctx->septa_label, cap, "ok", colorOnTime(), age, nullptr);
    }
  }
  // Weather: age_s is millis-based and -1 until the first success; stale past an hour (F29).
  if (ctx->weather_label != nullptr) {
    WeatherView wv = getWeather();
    if (wv.age_s < 0) {
      setSource(ctx->weather_label, nullptr, "no data yet", colorScheduled(), -1, nullptr);
    } else {
      setSource(ctx->weather_label, nullptr, wv.stale ? "stale" : "ok", wv.stale ? colorSkipped() : colorOnTime(), wv.age_s, nullptr);
    }
  }
  // Indego: 5 min cadence; the arrivals page's own strip turns amber past 10 min.
  if (ctx->bike_label != nullptr) {
    BikeView bv = getBikes();
    int32_t age = bv.fetched_epoch > 0 && clock_set && now >= bv.fetched_epoch ? (int32_t)(now - bv.fetched_epoch) : -1;
    if (bv.fetched_epoch == 0) {
      setSource(ctx->bike_label, nullptr, "no data yet", colorScheduled(), -1, nullptr);
    } else {
      bool stale = age > 10 * 60;
      setSource(ctx->bike_label, nullptr, stale ? "stale" : "ok", stale ? colorSkipped() : colorOnTime(), age, nullptr);
    }
  }
  // Alerts: 5 min cadence per route; stale once three of those have passed with no fetch.
  if (ctx->alerts_label != nullptr) {
    AlertsStatus as = getAlertsStatus();
    if (!as.fetched) {
      setSource(ctx->alerts_label, nullptr, "no data yet", colorScheduled(), -1, nullptr);
    } else {
      bool stale = as.age_s > 15 * 60;
      setSource(ctx->alerts_label, nullptr, stale ? "stale" : "ok", stale ? colorSkipped() : colorOnTime(), (int32_t)as.age_s, nullptr);
    }
  }

  uptimeText(millis() / 1000, buf, sizeof buf);
  lv_label_set_text(ctx->uptime_label, buf);
  lv_label_set_text(ctx->pin_label, auth::pin().c_str());

  // libc snprintf, not lv_label_set_text_fmt: LVGL's built-in printf (LV_USE_STDLIB_SPRINTF in
  // lv_conf.h) has no float support, and the first version of this line rendered as "f MB free".
  // Write health (sd_logger.h, F26) replaces the free-space figure: "mounted" alone is not the
  // claim that the rows are landing on the card. Red while writes are still failing, with the
  // logger's reason; amber once they land again.
  SdStatus sd = getSdStatus();
  if (!sd.mounted) {
    snprintf(buf, sizeof buf, "#%06x SD#  not mounted", sub);
  } else if (sd.dropped_rows > 0 && sd.last_write_ok) {
    snprintf(buf, sizeof buf, "#%06x SD#  #%06x %u rows dropped#, writing again", sub, (unsigned)colorHex(colorSkipped()),
             (unsigned)sd.dropped_rows);
  } else if (sd.dropped_rows > 0) {
    snprintf(buf, sizeof buf, "#%06x SD#  #%06x %u rows dropped%s%s#", sub, (unsigned)colorHex(colorLate()),
             (unsigned)sd.dropped_rows, sd.error.empty() ? "" : ": ", sd.error.c_str());
  } else {
    snprintf(buf, sizeof buf, "#%06x SD#  mounted, %.0f MB free", sub, (double)sd.free_bytes / (1024.0 * 1024.0));
  }
  lv_label_set_text(ctx->sd_label, buf);
  // MALLOC_CAP_8BIT, not ESP.getFreeHeap(): the panel and the web app's "Heap free" tile have to
  // agree, and the byte-addressable heap is the one a buffer can actually be given. ESP.getFreeHeap()
  // reads ~34 KB high here because it also counts 32-bit-word-only IRAM (DESIGN.md SS2.1).
  snprintf(buf, sizeof buf, "#%06x heap#  %u KB free", sub, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
  lv_label_set_text(ctx->heap_label, buf);
}

}  // namespace transit_app::ui
