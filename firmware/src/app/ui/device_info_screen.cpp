#include "device_info_screen.h"

#include <Arduino.h>
#include <WiFi.h>

#include "../auth.h"
#include "../sd_logger.h"
#include "ui_common.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

namespace transit_app::ui {

namespace {

constexpr uint32_t kWifiResetHoldMs = 5000;

struct DeviceInfoCtx {
  lv_obj_t *ip_label;
  lv_obj_t *mdns_label;
  lv_obj_t *ssid_label;
  lv_obj_t *rssi_label;
  lv_obj_t *pin_label;
  lv_obj_t *sd_label;
  lv_obj_t *heap_label;
  lv_obj_t *reset_label;
  std::string mdns_host;
  uint32_t press_start_ms = 0;
};

void resetTargetEventCb(lv_event_t *e) {
  auto *ctx = static_cast<DeviceInfoCtx *>(lv_event_get_user_data(e));
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    ctx->press_start_ms = millis();
    lv_label_set_text(ctx->reset_label, "hold to confirm...");
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    uint32_t held = millis() - ctx->press_start_ms;
    lv_label_set_text(ctx->reset_label, "reset Wi-Fi: hold 5 s");
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

lv_obj_t *addRow(lv_obj_t *parent, const lv_font_t *font) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, colorText(), 0);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  return l;
}

}  // namespace

lv_obj_t *createDeviceInfoScreen(const Config &cfg) {
  int32_t w, h;
  screenSize(w, h);

  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 10, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(screen, 4, 0);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);  // tap anywhere (except the reset target) cycles pages

  auto *ctx = new DeviceInfoCtx();
  ctx->mdns_host = cfg.device.name + ".local";

  lv_obj_t *title = addRow(screen, fontBody(h));
  lv_label_set_text_fmt(title, "Device Info - %s", FIRMWARE_VERSION);
  lv_obj_set_style_text_color(title, colorText(), 0);

  ctx->ip_label = addRow(screen, fontSmall(h));
  ctx->mdns_label = addRow(screen, fontSmall(h));
  ctx->ssid_label = addRow(screen, fontSmall(h));
  ctx->rssi_label = addRow(screen, fontSmall(h));
  // One of the two ways the owner learns the admin PIN (auth.h); the other is the serial console
  // at boot. Both need physical possession of the display, which is the point: there is no
  // network path to it. Printed here rather than hidden behind a gesture because someone who can
  // read this screen can already unplug the device, press-and-hold to wipe its Wi-Fi, or walk off
  // with it - the PIN is not what is protecting it from them.
  ctx->pin_label = addRow(screen, fontSmall(h));
  ctx->sd_label = addRow(screen, fontSmall(h));
  ctx->heap_label = addRow(screen, fontSmall(h));

  lv_obj_t *reset_target = lv_obj_create(screen);
  lv_obj_set_size(reset_target, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(reset_target, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(reset_target, 1, 0);
  lv_obj_set_style_border_color(reset_target, colorSubtext(), 0);
  lv_obj_set_style_pad_all(reset_target, 8, 0);
  lv_obj_set_style_radius(reset_target, 0, 0);  // LV_DRAW_SW_COMPLEX is 0: rounded rects are not drawn
  lv_obj_remove_flag(reset_target, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(reset_target, LV_OBJ_FLAG_CLICKABLE);
  ctx->reset_label = addRow(reset_target, fontSmall(h));
  lv_obj_set_style_text_color(ctx->reset_label, colorSubtext(), 0);
  lv_label_set_text(ctx->reset_label, "reset Wi-Fi: hold 5 s");
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
  lv_label_set_text_fmt(ctx->ip_label, "IP: %s", connected ? WiFi.localIP().toString().c_str() : "(not connected)");
  lv_label_set_text_fmt(ctx->mdns_label, "mDNS: http://%s/", ctx->mdns_host.c_str());
  lv_label_set_text_fmt(ctx->ssid_label, "SSID: %s", connected ? WiFi.SSID().c_str() : "(none)");
  lv_label_set_text_fmt(ctx->rssi_label, "RSSI: %d dBm", connected ? WiFi.RSSI() : 0);
  lv_label_set_text_fmt(ctx->pin_label, "Web PIN: %s", auth::pin().c_str());

  SdStatus sd = getSdStatus();
  if (sd.mounted) {
    lv_label_set_text_fmt(ctx->sd_label, "SD: mounted, %.0f MB free", (double)sd.free_bytes / (1024.0 * 1024.0));
  } else {
    lv_label_set_text(ctx->sd_label, "SD: not mounted");
  }

  lv_label_set_text_fmt(ctx->heap_label, "Free heap: %u KB", (unsigned)(ESP.getFreeHeap() / 1024));
}

}  // namespace transit_app::ui
