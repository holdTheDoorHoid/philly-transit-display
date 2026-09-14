#include "ui.h"

#include <esp32_smartdisplay.h>

#include <algorithm>
#include <ctime>

#include "../demo_data.h"
#include "../net_poller.h"
#include "device_info_screen.h"
#include "main_screen.h"
#include "stats_screen.h"
#include "ui_common.h"

namespace transit_app::ui {

namespace {

enum class Page { Main, Stats, DeviceInfo };

// DESIGN.md SS3: the demo Snapshot is kept for screen work with no Wi-Fi/SEPTA reachable
// (-DDEMO_DATA, off by default - see web_server.cpp's GET /api/state, which gates the same way).
transit::Snapshot currentSnapshot() {
#ifdef DEMO_DATA
  return buildDemoSnapshot((transit::Epoch)time(nullptr));
#else
  return getSnapshot();
#endif
}

lv_obj_t *g_main_screen = nullptr;
lv_obj_t *g_stats_screen = nullptr;
lv_obj_t *g_device_info_screen = nullptr;
lv_obj_t *g_wifi_setup_screen = nullptr;
lv_obj_t *g_wifi_setup_ssid_label = nullptr;
Config g_cfg;
Page g_page = Page::Main;
bool g_initialized = false;

void onScreenTapped(lv_event_t *e) {
  (void)e;
  switch (g_page) {
    case Page::Main:
      g_page = Page::Stats;
      lv_screen_load(g_stats_screen);
      refreshStatsScreen(g_stats_screen);
      break;
    case Page::Stats:
      g_page = Page::DeviceInfo;
      lv_screen_load(g_device_info_screen);
      refreshDeviceInfoScreen(g_device_info_screen);
      break;
    case Page::DeviceInfo:
      g_page = Page::Main;
      lv_screen_load(g_main_screen);
      break;
  }
}

}  // namespace

void init(const Config &cfg) {
  g_cfg = cfg;

  g_main_screen = createMainScreen(g_cfg);
  g_stats_screen = createStatsScreen(g_cfg);
  g_device_info_screen = createDeviceInfoScreen(g_cfg);

  lv_obj_add_event_cb(g_main_screen, onScreenTapped, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(g_stats_screen, onScreenTapped, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(g_device_info_screen, onScreenTapped, LV_EVENT_CLICKED, nullptr);

  g_page = Page::Main;
  refreshMainScreen(g_main_screen, g_cfg, currentSnapshot());
  lv_screen_load(g_main_screen);
  g_initialized = true;
}

void showWifiSetupScreen(const std::string &ap_name) {
  // DESIGN.md main.cpp task: "3 minute portal timeout then retry loop" -
  // this can be called more than once per boot, so the screen is built
  // once and reused (only the SSID label text is updated on repeat calls)
  // rather than leaking a new lv_obj_t tree on every retry.
  if (g_wifi_setup_screen == nullptr) {
    int32_t w, h;
    screenSize(w, h);

    lv_obj_t *screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, w, h);
    lv_obj_set_style_bg_color(screen, colorBg(), 0);
    lv_obj_set_style_pad_all(screen, 16, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, fontBody(h), 0);
    lv_obj_set_style_text_color(title, colorText(), 0);
    lv_label_set_text(title, "Wi-Fi setup");

    g_wifi_setup_ssid_label = lv_label_create(screen);
    lv_obj_set_style_text_font(g_wifi_setup_ssid_label, fontBig(h), 0);
    lv_obj_set_style_text_color(g_wifi_setup_ssid_label, colorEarly(), 0);

    lv_obj_t *hint = lv_label_create(screen);
    lv_obj_set_style_text_font(hint, fontSmall(h), 0);
    lv_obj_set_style_text_color(hint, colorSubtext(), 0);
    lv_obj_set_width(hint, lv_pct(90));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Connect a phone or laptop to this Wi-Fi network, then open http://192.168.4.1 to set up your home Wi-Fi.");

    g_wifi_setup_screen = screen;
  }

  lv_label_set_text(g_wifi_setup_ssid_label, ap_name.c_str());
  lv_screen_load(g_wifi_setup_screen);
}

void tick() {
  if (!g_initialized) {
    return;
  }
  switch (g_page) {
    case Page::Main:
      refreshMainScreen(g_main_screen, g_cfg, currentSnapshot());
      break;
    case Page::DeviceInfo:
      refreshDeviceInfoScreen(g_device_info_screen);
      break;
    case Page::Stats:
      // Stats are a 30-day rollup (DESIGN.md SS8); no need to re-stream the SD card at the same
      // ~1Hz cadence as the live arrivals screen. getStopSummary() itself is cached for 60s
      // (net_poller.cpp), so this just re-reads that cache while the page is visible.
      refreshStatsScreen(g_stats_screen);
      break;
  }
}

void applyBrightness(uint8_t percent) {
  percent = std::min<uint8_t>(percent, 100);
  smartdisplay_lcd_set_backlight((float)percent / 100.0f);
}

}  // namespace transit_app::ui
