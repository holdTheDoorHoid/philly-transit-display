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
#include <esp_heap_caps.h>

#include <ctime>
#include <new>

#include "app/auth.h"
#include "app/config_store.h"
#include "app/cxx_exception_pool.h"
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

// Set by the PUT /api/config callback (which runs on the async web server's task) and acted on
// in loop(). MDNS.end()/begin() and configTzTime() both talk to lwIP and the SNTP module from the
// calling task; doing that from the AsyncTCP task is how the SNTP/dns_clear_cache panic below was
// first hit, and mDNS restarts from a foreign task have their own history. So the callback only
// raises a flag and the main task does the work. Volatile, one-way, idempotent: no lock needed.
volatile bool g_apply_network_settings = false;
std::string g_applied_name;
std::string g_applied_tz;

// DESIGN.md SS6: device.name is the mDNS hostname and device.tz drives every local-time display.
// Both used to need a reboot to take effect after a save (review F31) - the web UI reported the
// new name while the device was still answering on the old one, and a timezone change left the
// clock, the quiet-hours window and the profile windows an hour out until someone power-cycled.
void applyNetworkSettings() {
  Config cfg = transit_app::getActiveConfig();
  if (cfg.device.name != g_applied_name) {
    MDNS.end();
    if (MDNS.begin(cfg.device.name.c_str())) {
      MDNS.addService("http", "tcp", 80);
      log_i("main: mDNS restarted at http://%s.local/", cfg.device.name.c_str());
    } else {
      log_e("main: mDNS.begin() failed after a rename");
    }
    g_applied_name = cfg.device.name;
  }
  if (cfg.device.tz != g_applied_tz) {
    // Not re-priming DNS here: the warm-up in setup() (see its comment) only has to happen once
    // per boot, and by now SNTP has a resolved server and is running.
    configTzTime(cfg.device.tz.c_str(), "pool.ntp.org");
    log_i("main: timezone applied: %s", cfg.device.tz.c_str());
    g_applied_tz = cfg.device.tz;
  }
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
  // free = MALLOC_CAP_INTERNAL (includes ~34 KB of 32-bit-only IRAM heap malloc never uses for
  // data); free8 = MALLOC_CAP_8BIT, the number an allocation can really get (net_poller.cpp).
  Serial.printf("[heap] %-10s free=%u free8=%u largest=%u\n", stage, (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void setup() {
  Serial.begin(115200);
  delay(200);
  transit_app::hwProbeEarly();

  smartdisplay_init();
  heapStage("display");
  // DESIGN.md SS12.1: what libstdc++'s emergency exception pool asked for at static-init, i.e.
  // which __cxx_eh_arena_size_get the linker kept - 2048 is cxx_exception_pool.cpp's, 0 would be the
  // SDK's. Whether the pool then really catches an OOM-while-throwing is what POST /api/debug/oom
  // proves; this line only shows the request was made. Always-on, like the [heap] stages above.
  Serial.printf("[heap] eh_pool    arena=%u\n", (unsigned)__cxx_eh_arena_size_get());

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
  transit_app::ui::applyRotation(cfg.device.rotation);  // panel-native is portrait; config picks the orientation
  transit_app::ui::applyInvert(cfg.device.invert_colors);
  transit_app::ui::setTheme(cfg.device.theme);
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

  // Idempotent: wifi_portal.cpp already called this as soon as it powered the radio up (auth.h
  // explains why it has to be after that, not in setup()). Kept here so the dependency is visible
  // at the point the web server and the device info screen - both of which read the PIN - are
  // about to start.
  transit_app::auth::begin();

  // Prime Arduino's DNS state before SNTP starts. NetworkManager::hostByName() calls
  // dns_clear_cache() from the CALLER's task the first time it sees an IP; if SNTP's own lookup of
  // pool.ntp.org is in flight at that moment, lwIP runs the SNTP callback without the core lock
  // and asserts in sys_untimeout (seen 2026-09-14 as a panic on the first poll after a reboot).
  // Resolving once here, with nothing pending, flips that state harmlessly and warms the cache.
  {
    IPAddress ntp_ip;
    WiFi.hostByName("pool.ntp.org", ntp_ip);
  }
  configTzTime(cfg.device.tz.c_str(), "pool.ntp.org");
  g_applied_tz = cfg.device.tz;

  if (MDNS.begin(cfg.device.name.c_str())) {
    MDNS.addService("http", "tcp", 80);
    log_i("main: mDNS up at http://%s.local/", cfg.device.name.c_str());
  } else {
    log_e("main: mDNS.begin() failed");
  }
  g_applied_name = cfg.device.name;

  transit_app::startWebServer([](bool data_changed) {
    transit_app::requestRepoll(data_changed);
    transit_app::ui::onConfigChanged(transit_app::getActiveConfig());  // applied on the LVGL task
    g_apply_network_settings = true;                                   // mDNS/timezone, applied in loop()
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
  transit_app::ui::logMemory();
  transit_app::startNetPoller(cfg.device.poll_seconds);
  heapStage("poller");

  log_i("main: setup complete, free heap %u bytes", (unsigned)ESP.getFreeHeap());
}

void loop() {
  // This is loopTask on core 1 - the LVGL / display task. It allocates: ui::tick() copies the
  // live Snapshot (vectors of arrivals and strings) out from under the poller's mutex, and LVGL
  // widget updates allocate from the heap too. C++ exceptions are on in this SDK (-fexceptions),
  // so a std::bad_alloc here that nothing catches is std::terminate = reboot - which is how a
  // burst of concurrent web requests, eating the heap while the UI tried to copy the snapshot,
  // rebooted the board (device suite, 2026-09-15, abort in Snapshot::operator= <- getSnapshot()
  // <- ui::tick()). A dropped frame is harmless; the next tick redraws. Never let it be fatal.
  try {
    pumpLvgl();

    if (g_apply_network_settings) {
      g_apply_network_settings = false;
      applyNetworkSettings();
    }

    static uint32_t last_ui_refresh_ms = 0;
    uint32_t now = millis();
    if (now - last_ui_refresh_ms >= 1000) {
      transit_app::ui::tick();
      last_ui_refresh_ms = now;
    }
  } catch (const std::bad_alloc &) {
    static uint32_t last_oom_log_ms = 0;
    uint32_t now = millis();
    if (now - last_oom_log_ms >= 5000) {  // rate-limit: under real pressure this can fire every tick
      Serial.printf("[main] out of memory in the display loop (free %u, largest %u); skipping this frame\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      last_oom_log_ms = now;
    }
  }
}
