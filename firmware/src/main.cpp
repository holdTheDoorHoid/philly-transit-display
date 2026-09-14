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
#include "app/http_fetch.h"
#include "app/hw_probe.h"
#include "app/net_poller.h"
#include "app/proxy_worker.h"
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

// Always-on heap trace at each setup stage (CORE_DEBUG_LEVEL=2 hides log_i), so a fragmented
// heap is visible over serial before it turns into a failed allocation.
void heapStage(const char *stage) {
  Serial.printf("[heap] %-10s free=%u largest=%u\n", stage, (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void setup() {
  Serial.begin(115200);
  delay(200);
  transit_app::hwProbeEarly();

  smartdisplay_init();
  heapStage("display");

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
  transit_app::setUseHttps(cfg.device.use_https);
  transit_app::ui::applyRotation(cfg.device.rotation);  // panel-native is portrait; config picks the orientation
  transit_app::hwProbeDisplay();
  heapStage("config");

  // Big long-lived objects first, while the heap is one contiguous block (a boot loop was traced
  // to this allocation failing after Wi-Fi + web server had fragmented the heap).
  if (!transit_app::preallocateTracker()) {
    log_e("main: could not allocate the arrival tracker; SD logging disabled");
  }
  transit_app::initNetPoller();
  transit_app::startProxyWorker();
  heapStage("tasks");

  transit_app::connectWifiOrPortal(wifiApName(), pumpLvgl);
  heapStage("wifi");

  configTzTime(cfg.device.tz.c_str(), "pool.ntp.org");

  if (MDNS.begin(cfg.device.name.c_str())) {
    MDNS.addService("http", "tcp", 80);
    log_i("main: mDNS up at http://%s.local/", cfg.device.name.c_str());
  } else {
    log_e("main: mDNS.begin() failed");
  }

  transit_app::startWebServer([]() {
    transit_app::setUseHttps(transit_app::getActiveConfig().device.use_https);
    transit_app::requestRepoll();
    transit_app::ui::onConfigChanged(transit_app::getActiveConfig());  // applied on the LVGL task
  });
  heapStage("web");

  transit_app::SdStatus sd = transit_app::mountSd();
  if (sd.mounted) {
    log_i("main: SD mounted, %.1f MB free", (double)sd.free_bytes / (1024.0 * 1024.0));
  } else {
    log_w("main: SD not mounted (no card, or unreadable) - logging disabled until one is inserted and the device reboots");
  }

  // RGB LED status (DESIGN.md SS3): steady-state is "off"; net_poller.cpp
  // flashes green on a good poll and holds red on a failed one from here on.
  transit_app::setStatusLed(LedState::Off);

  heapStage("sd");

  transit_app::ui::init(cfg);
  transit_app::ui::applyBrightness(cfg.device.brightness);
  heapStage("ui");
  transit_app::startNetPoller(cfg.device.poll_seconds);
  heapStage("poller");

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
