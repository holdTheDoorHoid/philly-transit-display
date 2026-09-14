// Philly Transit Display firmware entry point. DESIGN.md SS5.
//
// setup() brings up every subsystem in the order DESIGN.md's firmware
// skeleton task lays out: display/LVGL, LittleFS + config, Wi-Fi (captive
// portal on first boot), NTP, mDNS, the web server, the SD card, the RGB
// status LED, then the background tasks. loop() just pumps LVGL (see
// esp32_smartdisplay's own README, "Step 7") and ui::tick() at ~1 Hz.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp32_smartdisplay.h>
#include <esp_mac.h>

#include <ctime>

#include "app/config_store.h"
#include "app/hw_probe.h"
#include "app/net_poller.h"
#include "app/sd_logger.h"
#include "app/status_led.h"
#include "app/ui/ui.h"
#include "app/web_server.h"
#include "app/wifi_portal.h"

namespace {

using transit_app::Config;
using transit_app::LedState;

// Pumps LVGL's tick + timer handler. Shared between loop() and the Wi-Fi
// setup retry loop below, both of which need the display to keep rendering
// (esp32_smartdisplay's README documents exactly this manual-pump pattern;
// LV_USE_OS is NONE in firmware/include/lv_conf.h, so nothing does this for
// us automatically).
void pumpLvgl() {
  static uint32_t last_tick_ms = millis();
  uint32_t now = millis();
  lv_tick_inc(now - last_tick_ms);
  last_tick_ms = now;
  lv_timer_handler();
}

std::string wifiApName() {
  // WiFi.macAddress() returns zeros before the Wi-Fi driver is started, so read
  // the factory MAC from efuse directly (observed "TransitDisplay-0000" otherwise).
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[32];
  snprintf(buf, sizeof(buf), "TransitDisplay-%02X%02X", mac[4], mac[5]);
  return std::string(buf);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  transit_app::hwProbeEarly();

  smartdisplay_init();
  lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_90);  // panels are wired portrait; DESIGN.md SS3/SS8 want landscape
  transit_app::hwProbeDisplay();

  if (!LittleFS.begin(false)) {
    log_w("main: LittleFS mount failed, formatting");
    LittleFS.begin(true);
  }

  Config cfg;
  if (!transit_app::loadConfig(cfg)) {
    log_w("main: no valid config on LittleFS, writing defaults (DESIGN.md SS6)");
    cfg = transit_app::defaultConfig();
    transit_app::saveConfig(cfg);
  }
  transit_app::setActiveConfig(cfg);

  transit_app::connectWifiOrPortal(wifiApName(), pumpLvgl);

  configTzTime(cfg.device.tz.c_str(), "pool.ntp.org");

  if (MDNS.begin(cfg.device.name.c_str())) {
    MDNS.addService("http", "tcp", 80);
    log_i("main: mDNS up at http://%s.local/", cfg.device.name.c_str());
  } else {
    log_e("main: mDNS.begin() failed");
  }

  transit_app::startWebServer([]() {
    transit_app::requestRepoll();
    transit_app::ui::applyBrightness(transit_app::getActiveConfig().device.brightness);
  });

  transit_app::SdStatus sd = transit_app::mountSd();
  if (sd.mounted) {
    log_i("main: SD mounted, %.1f MB free", (double)sd.free_bytes / (1024.0 * 1024.0));
  } else {
    log_w("main: SD not mounted (no card, or unreadable) - logging disabled until one is inserted and the device reboots");
  }

  // RGB LED status (DESIGN.md SS3): steady-state is "off"; net_poller.cpp
  // flashes green on a good poll and holds red on a failed one from here on.
  transit_app::setStatusLed(LedState::Off);

  transit_app::ui::init(cfg);
  transit_app::ui::applyBrightness(cfg.device.brightness);
  transit_app::startNetPoller(cfg.device.poll_seconds);

  log_i("main: setup complete, free heap %u bytes", (unsigned)ESP.getFreeHeap());
}

void loop() {
  pumpLvgl();

  static uint32_t last_ui_refresh_ms = 0;
  uint32_t now = millis();
  if (now - last_ui_refresh_ms >= 1000) {
    transit_app::ui::tick();
    last_ui_refresh_ms = now;
  }
}
