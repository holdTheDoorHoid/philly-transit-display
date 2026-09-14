#include "ui.h"

#include <esp32_smartdisplay.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <ctime>

#include "../demo_data.h"
#include "../due_alert.h"
#include "../net_poller.h"
#include "../profiles.h"
#include "../weather_service.h"
#include "daypart_core/daypart.h"
#include "device_info_screen.h"
#include "main_screen.h"
#include "night_screen.h"
#include "stats_screen.h"
#include "ui_common.h"

namespace transit_app::ui {

namespace {

enum class Page { Main, Stats, DeviceInfo };  // Main is either the arrivals page or the night clock

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
lv_obj_t *g_night_screen = nullptr;
lv_obj_t *g_stats_screen = nullptr;
lv_obj_t *g_device_info_screen = nullptr;
lv_obj_t *g_wifi_setup_screen = nullptr;
lv_obj_t *g_wifi_setup_ssid_label = nullptr;
Config g_cfg;
Page g_page = Page::Main;
bool g_initialized = false;
bool g_night = false;             // night clock is up instead of the arrivals page
int g_active_profile = -2;        // profiles.h index, -2 = not yet evaluated
bool g_dimmed = false;            // quiet hours have the backlight down
uint32_t g_wake_until_ms = 0;     // touch during quiet hours: normal brightness until then
bool g_swallow_click = false;     // the press that woke the screen must not change page
int g_applied_brightness = -1;
std::vector<std::string> g_shown_keys;  // stops the arrivals page shows (profiles.h), per build
bool g_due_active = false;
volatile bool g_tap_requested = false;
UiDebug g_debug;  // written at the end of tick() under g_pending_mutex, read by the web task

// Config handed over from another task (web server); applied on the LVGL task in tick().
SemaphoreHandle_t g_pending_mutex = nullptr;
Config g_pending_cfg;
bool g_pending = false;

void onScreenTapped(lv_event_t *e);
void onScreenPressed(lv_event_t *e);

lv_display_rotation_t rotationEnum(uint16_t degrees) {
  switch (degrees) {
    case 90: return LV_DISPLAY_ROTATION_90;
    case 180: return LV_DISPLAY_ROTATION_180;
    case 270: return LV_DISPLAY_ROTATION_270;
    default: return LV_DISPLAY_ROTATION_0;
  }
}

void buildScreens() {
  g_active_profile = activeProfileIndex(g_cfg, time(nullptr));
  g_shown_keys.clear();
  for (const transit::StopConfig &s : visibleStops(g_cfg, time(nullptr))) g_shown_keys.push_back(s.key);
  g_main_screen = createMainScreen(g_cfg);
  g_night_screen = createNightScreen(g_cfg);
  g_stats_screen = createStatsScreen(g_cfg);
  g_device_info_screen = createDeviceInfoScreen(g_cfg);
  for (lv_obj_t *scr : {g_main_screen, g_night_screen, g_stats_screen, g_device_info_screen}) {
    lv_obj_add_event_cb(scr, onScreenPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(scr, onScreenTapped, LV_EVENT_CLICKED, nullptr);
  }
}

// Loads the arrivals page or the night clock, whichever the data calls for (Page::Main only).
void showMainOrNight(const transit::Snapshot &snap) {
  bool night = nightConditionMet(g_cfg, snap, g_shown_keys, (transit::Epoch)time(nullptr));
  lv_obj_t *want = night ? g_night_screen : g_main_screen;
  if (lv_screen_active() != want) lv_screen_load(want);
  g_night = night;
  if (night) {
    refreshNightScreen(g_night_screen, g_cfg, snap);
  } else {
    refreshMainScreen(g_main_screen, g_cfg, snap);
  }
}

// DESIGN.md SS6 "quiet": backlight schedule with wake-on-touch. Returns true while dimmed.
bool applyQuietHours() {
  const QuietConfig &q = g_cfg.device.quiet;
  bool quiet = false;
  if (q.enabled) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    quiet = now >= 1700000000 && daypart::inWindow(lt.tm_hour * 60 + lt.tm_min, daypart::parseClock(q.start), daypart::parseClock(q.end));
  }
  bool awake = (int32_t)(millis() - g_wake_until_ms) < 0;
  bool dim = quiet && !awake;
  int target = dim ? q.brightness : g_cfg.device.brightness;
  if (target != g_applied_brightness) {
    applyBrightness((uint8_t)target);
    g_applied_brightness = target;
  }
  g_dimmed = dim;
  return dim;
}

// Tears down and recreates every screen (rotation, theme, ticker or stop list changed). A blank
// screen is loaded first because LVGL will not delete the active screen. Each screen frees its
// own context struct from an LV_EVENT_DELETE handler.
void rebuildScreens() {
  lv_obj_t *blank = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(blank, colorBg(), 0);
  lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
  lv_screen_load(blank);
  if (g_main_screen) lv_obj_delete(g_main_screen);
  if (g_night_screen) lv_obj_delete(g_night_screen);
  if (g_stats_screen) lv_obj_delete(g_stats_screen);
  if (g_device_info_screen) lv_obj_delete(g_device_info_screen);
  buildScreens();
  g_page = Page::Main;
  showMainOrNight(currentSnapshot());
  lv_obj_delete(blank);
}

void onScreenPressed(lv_event_t *e) {
  (void)e;
  if (g_dimmed) {
    // Wake for wake_seconds; the click that follows this press must not cycle pages.
    g_wake_until_ms = millis() + (uint32_t)g_cfg.device.quiet.wake_seconds * 1000u;
    g_swallow_click = true;
    applyQuietHours();
  } else if ((int32_t)(millis() - g_wake_until_ms) < 0) {
    g_wake_until_ms = millis() + (uint32_t)g_cfg.device.quiet.wake_seconds * 1000u;  // keep it awake
  }
}

void onScreenTapped(lv_event_t *e) {
  (void)e;
  if (g_swallow_click) {
    g_swallow_click = false;
    return;
  }
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
      showMainOrNight(currentSnapshot());
      break;
  }
}

}  // namespace

void init(const Config &cfg) {
  g_cfg = cfg;
  setTheme(g_cfg.device.theme);
  if (g_pending_mutex == nullptr) g_pending_mutex = xSemaphoreCreateMutex();

  buildScreens();

  g_page = Page::Main;
  showMainOrNight(currentSnapshot());
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
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
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
  // Apply a configuration handed over by the web server task (rotation, brightness, stops).
  bool apply = false;
  Config next;
  if (g_pending && g_pending_mutex && xSemaphoreTake(g_pending_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (g_pending) {
      next = g_pending_cfg;
      g_pending = false;
      apply = true;
    }
    xSemaphoreGive(g_pending_mutex);
  }
  if (apply) {
    bool rotate = next.device.rotation != g_cfg.device.rotation;
    bool invert = next.device.invert_colors != g_cfg.device.invert_colors;
    g_cfg = next;
    setTheme(g_cfg.device.theme);  // rebuildScreens() below re-reads every colour
    if (rotate) applyRotation(g_cfg.device.rotation);
    if (invert) applyInvert(g_cfg.device.invert_colors);
    g_applied_brightness = -1;  // applyQuietHours() below re-applies whichever brightness applies
    rebuildScreens();
  }

  // Commute profiles (profiles.h): the visible stop list changed -> rebuild the pages.
  int profile = activeProfileIndex(g_cfg, time(nullptr));
  if (profile != g_active_profile) {
    g_active_profile = profile;
    rebuildScreens();
  }

  if (g_tap_requested) {  // POST /api/debug/tap: the same two events a finger produces
    g_tap_requested = false;
    onScreenPressed(nullptr);
    onScreenTapped(nullptr);
  }

  bool dimmed = applyQuietHours();
  transit::Snapshot snap = currentSnapshot();
  g_due_active = dueAlertTick(g_cfg, snap, g_shown_keys, dimmed, (transit::Epoch)time(nullptr));

  switch (g_page) {
    case Page::Main:
      showMainOrNight(snap);
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

  UiDebug d;
  d.page = g_page == Page::Stats ? "stats" : g_page == Page::DeviceInfo ? "device" : (g_night ? "night" : "main");
  d.dimmed = dimmed;
  d.brightness = g_applied_brightness;
  d.due_active = g_due_active;
  d.chimes = dueChimesPlayed();
  d.active_profile = g_active_profile >= 0 && (size_t)g_active_profile < g_cfg.profiles.size() ? g_cfg.profiles[(size_t)g_active_profile].name : "";
  d.shown_stops = g_shown_keys;
  mainScreenDebug(g_main_screen, d.hidden_panels, d.ticker, d.rows);
  d.header_weather = g_cfg.weather.enabled ? headerWeatherText() : "";
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  d.lv_used = m.total_size - m.free_size;
  d.lv_free = m.free_size;
  d.lv_max_used = m.max_used;
  lv_display_t *disp = lv_display_get_default();
  d.hor_res = lv_display_get_horizontal_resolution(disp);
  d.ver_res = lv_display_get_vertical_resolution(disp);
  if (g_pending_mutex && xSemaphoreTake(g_pending_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    g_debug = d;
    xSemaphoreGive(g_pending_mutex);
  }
}

UiDebug debugSnapshot() {
  UiDebug d;
  if (g_pending_mutex && xSemaphoreTake(g_pending_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    d = g_debug;
    xSemaphoreGive(g_pending_mutex);
  }
  return d;
}

void requestTap() {
  g_tap_requested = true;
}

void applyBrightness(uint8_t percent) {
  percent = std::min<uint8_t>(percent, 100);
  smartdisplay_lcd_set_backlight((float)percent / 100.0f);
}

void applyRotation(uint16_t degrees) {
  lv_display_t *d = lv_display_get_default();
  if (d == nullptr) return;
  lv_display_set_rotation(d, rotationEnum(degrees));
}

void applyInvert(bool invert) {
  lv_display_t *d = lv_display_get_default();
  if (d == nullptr) return;
  // esp32_smartdisplay stores the esp_lcd panel handle in the display's user_data
  // (lvgl_panel_st7796_spi.c / lvgl_panel_ili9341_spi.c). The library itself only sends the
  // inversion command when a board file defines DISPLAY_IPS, which none of the vendored Sunton
  // files do; the owner's 3.5" panel needs it (docs/hardware.md).
  auto panel = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(d));
  if (panel == nullptr) return;
  esp_lcd_panel_invert_color(panel, invert);
}

void onConfigChanged(const Config &cfg) {
  if (g_pending_mutex == nullptr) return;  // before init(): main.cpp applies the boot config itself
  if (xSemaphoreTake(g_pending_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    g_pending_cfg = cfg;
    g_pending = true;
    xSemaphoreGive(g_pending_mutex);
  }
}

void logMemory() {
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  Serial.printf("[lvmem] total=%u free=%u used_pct=%u frag_pct=%u max_used=%u\n", (unsigned)m.total_size, (unsigned)m.free_size, (unsigned)m.used_pct, (unsigned)m.frag_pct, (unsigned)m.max_used);
}

}  // namespace transit_app::ui
