// Device page in the main page's visual language (DESIGN.md SS8): the header strip with the
// firmware version, a "Network" panel (Wi-Fi bars, SSID, the mDNS URL - how the owner reaches
// the web app - the IP, and whether the last SEPTA poll worked), a panel named after the device
// (the web PIN in the big font, SD status and write health, heap, uptime), and the
// hold-5-seconds Wi-Fi reset as a bordered button along the bottom.
#include "device_info_screen.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>
#include <ctime>
#include <string>

#include "../auth.h"
#include "../net_poller.h"
#include "../sd_logger.h"
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
  lv_obj_t *septa_label;
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
  ctx->septa_label = captionedLine(net, h);

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
    bool big = h >= 320 && auth::pin().size() <= 8;
    ctx->pin_label = makeLabel(row, big ? fontBig(h) : fontBody(h), colorText());
    lv_label_set_long_mode(ctx->pin_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(ctx->pin_label, lv_pct(75), 0);
    lv_label_set_text(ctx->pin_label, "");
    lv_obj_t *cap = makeLabel(row, fontSmall(h), colorSubtext());
    if (big) lv_obj_set_style_pad_bottom(cap, (lv_font_get_line_height(fontBig(h)) - lv_font_get_line_height(fontSmall(h))) / 6, 0);
    lv_label_set_text(cap, "web PIN");
  }
  ctx->sd_label = captionedLine(dev, h);
  ctx->heap_label = captionedLine(dev, h);

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
  lv_obj_set_style_pad_all(reset_target, h >= 320 ? 8 : 6, 0);
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

  // "SEPTA  ok, 12 s ago" / "SEPTA  failed 4 min ago: HTTP 503". The age is left off until the
  // clock has been set (an epoch before 2023 is the boot default, not a time). Every line here is
  // composed with snprintf and inline recolor commands (ui_common.h colorHex), not std::string.
  char buf[96];
  unsigned sub = (unsigned)colorHex(colorSubtext());
  {
    PollStatus ps = getPollStatus();
    uint32_t now = (uint32_t)time(nullptr);
    char age[16] = "";
    if (ps.has_polled && now > 1700000000u && now >= ps.last_poll_epoch) agoText(now - ps.last_poll_epoch, age, sizeof age);
    if (!ps.has_polled) {
      snprintf(buf, sizeof buf, "#%06x SEPTA#  no poll yet", sub);
    } else if (ps.ok) {
      snprintf(buf, sizeof buf, "#%06x SEPTA#  #%06x ok#%s%s", sub, (unsigned)colorHex(colorOnTime()), age[0] ? ", " : "", age);
    } else {
      snprintf(buf, sizeof buf, "#%06x SEPTA#  #%06x failed#%s%s%s%s", sub, (unsigned)colorHex(colorLate()),
               age[0] ? " " : "", age, ps.last_error.empty() ? "" : ": ", ps.last_error.c_str());
    }
    lv_label_set_text(ctx->septa_label, buf);
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
  snprintf(buf, sizeof buf, "#%06x heap#  %u KB free", sub, (unsigned)(ESP.getFreeHeap() / 1024));
  lv_label_set_text(ctx->heap_label, buf);
}

}  // namespace transit_app::ui
