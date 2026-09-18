// Philly Transit Display firmware entry point. DESIGN.md SS5.
//
// setup() brings up every subsystem in the order DESIGN.md's firmware
// skeleton task lays out: display/LVGL, LittleFS + config, Wi-Fi (captive
// portal on first boot), NTP, mDNS, the SD card, the web server, the RGB
// status LED, then the background tasks. loop() just pumps LVGL (see
// esp32_smartdisplay's own README, "Step 7") and ui::tick() at ~1 Hz.
//
// The SD card is mounted BEFORE the web server since 0.3.1: that mount is one
// ~12.5 KB contiguous calloc and this board's constraint is the largest free
// block, so it goes first (see its own comment below, audit_static SS9.7).
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp32_smartdisplay.h>
#include <esp_mac.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>

#include <ctime>
#include <new>

#include "app/auth.h"
#include "app/bike_service.h"
#include "app/config_store.h"
#include "app/cxx_exception_pool.h"
#include "app/http_fetch.h"
#include "app/heap_reserve.h"
#include "app/hw_probe.h"
#include "app/net_poller.h"
#include "app/nightly_restart.h"
#include "daypart_core/daypart.h"
#include "app/poller_liveness.h"
#include "app/proxy_worker.h"
#include "app/sd_logger.h"
#include "app/status_led.h"
#include "app/ui/ui.h"
#include "app/ui_lock.h"
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
  // tryGetActiveConfig(), not getActiveConfig(): this runs on the display task, where the read does
  // not wait (app/ui_lock.h), and an empty Config on a miss would rename mDNS to "" and set an empty
  // timezone. On a miss the flag stays raised and loop() tries again on its next pass.
  Config cfg;
  if (!transit_app::tryGetActiveConfig(&cfg)) {
    g_apply_network_settings = true;
    return;
  }
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

// DESIGN.md SS12.1: the poller-liveness net. The poller stamps net_poller.cpp's g_cycle_end_ms at
// the end of every cycle whatever the outcome, and g_progress_ms at the start of every fetch it
// makes; this asks, once a second from the DISPLAY task, whether the later of the two has gone
// stale, and restarts the board if it has.
//
// Why here. A poller that has stopped cannot notice that it has stopped, so the check has to run
// somewhere else. loopTask is the right somewhere: it is on the other core, it already runs at
// ~1 Hz unconditionally, it has its own bad_alloc guard, and it needs no stack or task of its own
// (a dedicated watchdog task would cost ~2 KB of a heap this file exists to husband). The two
// alternatives were each worse: the AsyncTCP task only runs when someone makes a request, so a
// display nobody is browsing would never be checked; and subscribing the poller to the ESP-IDF
// task watchdog cannot work here, because that watchdog has ONE global timeout (5 s in this SDK)
// and a poll interval is 5-600 s, so the poller could never feed it.
//
// The check is outside loop()'s try/catch and allocates nothing - volatile reads, a stack buffer,
// Serial.println - because the moment it matters most is the moment the heap is gone and
// ui::tick() is throwing. Serial.printf() would malloc for a line this long; snprintf into a
// stack buffer cannot.
void checkPollerLiveness() {
  using transit_app::PollerLiveness;

  static uint32_t last_check_ms = 0;
  static uint32_t ota_seen_ms = 0;
  uint32_t now = millis();
  if (now - last_check_ms < 1000) return;
  last_check_ms = now;

  // HAZARD: a firmware upload. An OTA legitimately starves the poller (it is writing ~1.7 MB to
  // flash on the other task and taking the heap while it does). A reboot mid-write is not a brick
  // and never was - esp_ota_set_boot_partition() runs inside Update.end(true), so an interrupted
  // upload leaves a half-written INACTIVE slot and the device comes back on the image it is
  // already running - but it throws the owner's upload away at the worst possible moment and looks
  // exactly like a crash to whoever is watching the progress bar. So: never judge while one is
  // running - and not for a full window after it ends either, so an upload that has just finished
  // or just been aborted cannot be followed straight away by a reboot the stall timer had already
  // earned during it. net_poller.cpp's heap-wedge counter stands down for the same reason.
  if (transit_app::otaBusy()) {
    ota_seen_ms = now;
    if (ota_seen_ms == 0) ota_seen_ms = 1;  // 0 is the "never seen" sentinel
    return;
  }

  PollerLiveness lv = transit_app::getPollerLiveness();
  // HAZARD: setup and AP mode. `armed` is set by startNetPoller(), the last thing setup() does, so
  // it is false for the whole captive-portal / no-credentials path - during which loop() is not
  // running anyway, because connectWifiOrPortal() does not return until Wi-Fi is up.
  if (!lv.armed) return;

  const uint32_t window_ms = transit_app::pollerStallTimeoutMs(lv.interval_ms, lv.before_first_cycle);
  if (ota_seen_ms != 0) {
    if (now - ota_seen_ms < window_ms) return;
    ota_seen_ms = 0;  // grace spent; forget it rather than carry it to the millis() wrap
  }
  // idle_ms, not since_ms (poller_liveness.h): the poller stamps every fetch it starts as well as
  // every cycle it finishes, so a cycle that is legitimately taking minutes on a blackholing
  // network keeps the clock moving. Judging on cycle completion alone is what let a device with
  // several configured stops reboot itself mid-cycle, over and over.
  if (!transit_app::pollerHasStalled(lv.idle_ms, lv.interval_ms, lv.before_first_cycle)) return;

  // A device whose network is simply down does NOT reach here: a failed fetch is still a completed
  // cycle, and a fetch still in flight is still a stamp, so the clock keeps moving and only the
  // backoff changes. Getting here means the poller did nothing at all - no cycle, no fetch, not
  // even a failure - for several whole intervals.
  char line[192];
  snprintf(line, sizeof(line),
           "[main] poller stalled: nothing for %u s (last cycle %u s ago, interval %u s, window %u s, "
           "free %u, largest %u); restarting",
           (unsigned)(lv.idle_ms / 1000U), (unsigned)(lv.since_ms / 1000U),
           (unsigned)(lv.interval_ms / 1000U), (unsigned)(window_ms / 1000U),
           (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.println(line);
  transit_app::noteSelfHealRestart(transit_app::SelfHeal::PollStall, lv.idle_ms / 1000U, lv.interval_ms / 1000U);
  Serial.flush();
  delay(200);
  ESP.restart();
}

// DESIGN.md SS12.1 / nightly_restart.h: the deliberate restart, checked once a minute from the
// display task. Nothing here allocates or blocks; the setting is two aligned words the poller
// republishes every cycle, and the clock read is localtime_r on a stack struct.
//
// It runs OUTSIDE loop()'s bad_alloc guard, alongside checkPollerLiveness() and for the same
// reason: the frame the display loop skips because it could not allocate is exactly the frame in
// which this is most worth doing.
void checkNightlyRestart() {
  static int last_minute = -1;
  if (!transit_app::nightlyRestartEnabled()) return;
  time_t now = time(nullptr);
  struct tm lt;
  // Before NTP the local time is 1970 and "03:30" would match at a moment that has nothing to do
  // with 03:30. The same threshold net_poller.cpp's clockIsSane() uses (kSaneClockEpoch,
  // 2023-11-14): repeated rather than exported, because that one is a file-local helper with five
  // other callers inside its own translation unit and widening its scope for this would be the
  // larger change.
  constexpr time_t kSaneClockEpoch = 1700000000;
  if (now < kSaneClockEpoch || localtime_r(&now, &lt) == nullptr) return;
  const int now_minute = lt.tm_hour * 60 + lt.tm_min;
  const uint32_t uptime_s = (uint32_t)(millis() / 1000U);
  const bool fire = transit_app::shouldRestartNightly(
    true, transit_app::nightlyRestartMinute(), now_minute, last_minute, uptime_s,
    transit_app::otaBusy());
  // Stamped whether or not it fired, so the rule's "once a minute" guard holds: a minute that was
  // considered and declined must not be considered again forty passes later.
  last_minute = now_minute;
  if (!fire) return;
  Serial.printf("[restart] nightly restart at %02d:%02d\n", lt.tm_hour, lt.tm_min);
  // Recorded before the restart is scheduled, so GET /api/state's last_restart says "nightly"
  // rather than leaving an unexplained ESP_RST_SW on the next boot. `a` is the minute of day it
  // was set to, `b` the uptime in seconds it reached - both useful if it ever fires at the wrong
  // time.
  transit_app::noteSelfHealRestart(transit_app::SelfHeal::Nightly, (uint32_t)now_minute, uptime_s);
  Serial.flush();
  transit_app::scheduleRestart();
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

// Hands libbt's static data back to the heap, before anything else has allocated (audit_static
// SS4.1, top-10 item 3). This is NOT esp_bt_controller_mem_release(), which Arduino's
// initArduino() already calls unconditionally before setup() (esp32-hal-misc.c:309, via the weak
// btInUse() stub - no BT library is linked here) and which has therefore already given back the
// four BT ROM regions; calling that again gains nothing. esp_bt_mem_release() does all of that
// AND `_bt_bss` + `_bt_data`, and on this image `_bt_data` is 4,464 bytes of .dram0.data -
// libbt's hli_vectors table (4,380 B) plus 84 B from bt.c - that nothing releases today. On a
// board whose largest free block is the resource that runs out, a new ~4.4 KB contiguous heap
// region is not a rounding error.
//
// Read from the disassembly rather than from the docs, and stated honestly: the call first re-runs
// the controller/ROM release (idempotent - esp_bt_controller_rom_mem_release keeps a bitmask of
// what is left) and then returns early on a non-zero result from the first areas pair, both
// members of which are below heap_caps_add_region()'s minimum and yield ESP_ERR_INVALID_SIZE,
// which esp_bt_mem_release_area maps to ESP_OK. The static analysis says it succeeds; this has
// never run on this hardware. So the device says so itself: the return code and the 8-bit free
// heap either side of the call go on the boot log, and if it fails the failure mode is "no gain",
// not a crash. It is irreversible (no BT this boot) - which is correct here: nothing in this
// firmware uses Bluetooth, and DESIGN.md SS11 has no plan to.
void releaseBluetoothMemory() {
#if defined(CONFIG_BT_ENABLED) && defined(SOC_BT_SUPPORTED)
  const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
  const size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const int32_t gain = (int32_t)after - (int32_t)before;
  Serial.printf("[heap] bt_release err=%d (%s) free8 %u -> %u (%+d) largest=%u\n", (int)err,
                esp_err_to_name(err), (unsigned)before, (unsigned)after, (int)gain,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  // ...and where it can be READ: the serial console cannot be captured on this bench (opening the
  // port resets the board), so GET /api/debug/ui carries the same two numbers.
  transit_app::noteBluetoothRelease((int)err, gain);
#else
  Serial.println("[heap] bt_release skipped: no Bluetooth controller in this build");
  transit_app::noteBluetoothRelease(-1, 0);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(200);
  // First, before the display's 15 KB draw buffer and everything after it: whatever this returns
  // to the heap is most useful while the heap is still one run.
  releaseBluetoothMemory();
  transit_app::hwProbeEarly();

  smartdisplay_init();
  heapStage("display");
  // DESIGN.md SS12.1: what libstdc++'s emergency exception pool asked for at static-init, i.e.
  // which __cxx_eh_arena_size_get the linker kept - 2048 is cxx_exception_pool.cpp's, 0 would be the
  // SDK's. Whether the pool then really catches an OOM-while-throwing is what POST /api/debug/oom
  // proves; this line only shows the request was made. Always-on, like the [heap] stages above.
  Serial.printf("[heap] eh_pool    arena=%u\n", (unsigned)__cxx_eh_arena_size_get());
  // DESIGN.md SS12.1, second half: the pool covers the exception OBJECT, not the per-task
  // __cxa_eh_globals that __cxa_throw allocates with a plain malloc on a task's FIRST throw - which
  // terminates outright if the heap is gone by then. Warm it here, on loopTask, while the heap is
  // untouched; this is the same task loop() runs on, so the LVGL display loop's catch below is
  // covered by it. The poller and the AsyncTCP task each warm their own (they are other tasks, and
  // the storage is per-task).
  transit_app::warmExceptionGlobals("loopTask");

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
  // The poll cycle's working set, for exactly the same reason and at exactly the same moment
  // (DESIGN.md SS5 "the poll working set"): the GTFS-RT entity and retention buffers, the response
  // body buffers and the schedule parse block are each a multi-kilobyte CONTIGUOUS request, and on
  // this board the largest free block - not the free heap - is what runs out. A failure here is a
  // slower poll, not a broken one: transit_core builds them per call as it always did.
  if (!transit_app::preallocatePollBuffers()) {
    log_e("main: could not reserve the poll working set; each cycle will allocate its own");
  }
  if (!transit_app::preallocateBikeStream(transit_app::pollScratch())) {
    log_e("main: could not reserve the Indego feed scanner; each refresh will allocate its own");
  }
  // The kilobyte that lets the device still SAY "out of memory" when it is out of memory
  // (app/heap_reserve.h: a coredump on 2026-09-17 caught guarded()'s own 503 throwing out of its
  // catch handler and terminating). Armed here, with the others, before Wi-Fi.
  Serial.printf("[heap] oom_reserve %s (%u B)\n",
                transit_app::armHeapReserve() ? "armed" : "NOT ARMED (out of memory)",
                (unsigned)transit_app::kHeapReserveBytes);
  transit_app::initNetPoller();
  transit_app::startProxyWorker();
  heapStage("tasks");

  transit_app::connectWifiOrPortal(wifiApName(), pumpLvgl);
  heapStage("wifi");

  // The fourth task that can throw, and the only one we do not create: lwIP's own "tiT"
  // (0.3.2-rc4, app/cxx_exception_pool.cpp has the decoded backtrace). AsyncTCP's lwIP raw
  // callbacks - tcp_poll, tcp_recv, tcp_sent, tcp_error, tcp_accept, the DNS callback - all run
  // there and all allocate with `new (std::nothrow)`, which libstdc++ implements as a try/catch
  // around the THROWING new. So the first allocation failure in AsyncTCP's lwIP half is a real
  // throw on a task that has never thrown, and it terminates on the ~16 B malloc inside
  // __cxa_get_globals with the heap already gone. This warms it through lwIP's own callback
  // mechanism, which is the only way onto that task.
  //
  // HERE and not earlier: the tcpip task does not exist until the TCP/IP stack is initialised,
  // which connectWifiOrPortal() above is what causes. Both branches of that call (joined a network,
  // or brought up the setup AP) start the stack, so the task is there either way.
  transit_app::warmExceptionGlobalsOnTcpipTask();

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
  // The nightly restart is armed from the loaded config here, not left until the first poll cycle
  // republishes it (nightly_restart.h). It could not fire for the first hour either way - the
  // uptime guard sees to that - but the setting should be true from boot rather than default.
  transit_app::setNightlyRestart(cfg.device.nightly_restart.enabled,
                                 daypart::parseClock(cfg.device.nightly_restart.time));
  if (cfg.device.nightly_restart.enabled) {
    Serial.printf("[restart] nightly restart armed for %s local\n",
                  cfg.device.nightly_restart.time.c_str());
  }

  if (MDNS.begin(cfg.device.name.c_str())) {
    MDNS.addService("http", "tcp", 80);
    log_i("main: mDNS up at http://%s.local/", cfg.device.name.c_str());
  } else {
    log_e("main: mDNS.begin() failed");
  }
  g_applied_name = cfg.device.name;

  // BEFORE the web server, since 0.3.1 (audit_static SS9.7). Mounting the card is one
  // ~12.5 KB CONTIGUOUS calloc - esp_vfs_fat_register() allocates the FATFS context and both FIL
  // slots in a single block (sizeof(FATFS) 4,152 + 2 x sizeof(FIL) 4,136, measured) - and it is the
  // largest single allocation this device makes after boot. Asking for it after the web server has
  // started meant asking for it from a heap the AsyncWebServer had already been allocating out of;
  // here it comes off one that is still close to one run. Nothing in startWebServer() needs the
  // card: its SD-backed routes (/api/log/*, /api/stats) only touch sd_logger at request time, long
  // after this. The `[heap] sd-pre` / `[heap] sd-begin` pair printed inside mountSd() is what says
  // whether the block was there, and the stage lines below still bracket what they name.
  transit_app::SdStatus sd = transit_app::mountSd();
  if (sd.mounted) {
    log_i("main: SD mounted, %.1f MB free", (double)sd.free_bytes / (1024.0 * 1024.0));
  } else {
    log_w("main: SD not mounted (no card, or unreadable) - logging disabled until one is inserted and the device reboots");
  }
  heapStage("sd");

  transit_app::startWebServer([](bool data_changed) {
    transit_app::requestRepoll(data_changed);
    transit_app::ui::onConfigChanged();  // one flag; the LVGL task picks up the published pointer
    g_apply_network_settings = true;                                   // mDNS/timezone, applied in loop()
  });
  heapStage("web");

  // RGB LED status (DESIGN.md SS3): steady-state is "off"; net_poller.cpp
  // flashes green on a good poll and holds red on a failed one from here on.
  transit_app::setStatusLed(LedState::Off);

  // The published pointer, not a copy of `cfg`: the display task borrows config_store's one
  // Config for the life of the device instead of keeping a second resident one (config_store.h,
  // "the active config is one object, published by pointer"). setActiveConfig(cfg) ran above.
  transit_app::ui::init(transit_app::activeConfigPtr());
  transit_app::ui::applyBrightness(cfg.device.brightness);
  heapStage("ui");
  transit_app::ui::logMemory();
  // From here on this task is THE display task, and every shared accessor it calls stops waiting on
  // other tasks' locks (app/ui_lock.h, DESIGN.md SS5). Deliberately the last thing before the poller
  // exists: everything above ran with nothing to contend with, and wanted its answers.
  transit_app::noteDisplayTask();
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

  // Outside the guard on purpose (DESIGN.md SS12.1): the frame the display loop skips because it
  // could not allocate is exactly the frame in which the poller is most likely to be stuck, so the
  // liveness check must not be skipped with it. Nothing in it allocates.
  checkPollerLiveness();
  checkNightlyRestart();
}
