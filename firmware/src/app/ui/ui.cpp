#include "ui.h"

#include <esp32_smartdisplay.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <ctime>
#include <memory>
#include <utility>

#include "../demo_data.h"
#include "../due_alert.h"
#include "../net_poller.h"
#include "../profiles.h"
#include "../ui_lock.h"
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
//
// A POINTER, not a copy (2026-09-16). This ran once a second and copied the whole live Snapshot -
// every arrival, every string - out from under the poller's mutex, which is both the allocation
// that aborted the board on 2026-09-15 (main.cpp loop()) and, with the lock held for its whole
// duration, a large part of why anyone waiting on that lock waited long enough to matter. The
// poller now publishes an immutable shared_ptr and this borrows it: no copy, no allocation, and
// the time under the lock is one refcount bump.
std::shared_ptr<const transit::Snapshot> currentSnapshot() {
#ifdef DEMO_DATA
  return std::make_shared<const transit::Snapshot>(buildDemoSnapshot((transit::Epoch)time(nullptr)));
#else
  return snapshotPtr();
#endif
}

// What the screens render before the first poll has published anything, or if the display task has
// never once got the lock. A file-scope const with empty vectors: no heap, and every screen already
// renders an empty Snapshot as "no arrivals" rather than as a fault.
const transit::Snapshot kNoSnapshot;

// The four pages, of which exactly one is ever built (see the pool note above buildSlot()).
lv_obj_t *g_main_screen = nullptr;
lv_obj_t *g_night_screen = nullptr;
lv_obj_t *g_stats_screen = nullptr;
lv_obj_t *g_device_info_screen = nullptr;
// An empty screen that owns nothing, built once at init() and never freed. Every transition parks
// on it so the page being left can be deleted BEFORE the next one is built - LVGL refuses to
// delete the active screen (it warns and nulls act_scr), and building first is exactly the peak
// this rework exists to remove. It also carries the one message below.
lv_obj_t *g_parking_screen = nullptr;
lv_obj_t *g_parking_label = nullptr;
lv_obj_t *g_wifi_setup_screen = nullptr;
lv_obj_t *g_wifi_setup_ssid_label = nullptr;
lv_obj_t *g_wifi_setup_pass_label = nullptr;
lv_obj_t *g_wifi_setup_qr = nullptr;
lv_obj_t *g_connecting_screen = nullptr;       // "Connecting to <ssid>..." while retrying stored creds
lv_obj_t *g_connecting_ssid_label = nullptr;
lv_obj_t *g_connecting_detail_label = nullptr;
// THE CONFIG THE SCREENS RENDER FROM - a borrowed pointer, not a second resident copy
// (0.3.2-rc3, runtime audit rec #8). config_store publishes one shared_ptr<const Config>; this
// task holds a reference to that same object, so the device keeps ONE Config in memory instead of
// three (the store's, the pending handover's and this one). What a save costs is now one Config
// allocated and one freed, rather than four copies each scattering ~12 small string blocks into
// new holes - which is the churn the device suite's config section provokes.
//
// Never null after init(); cfg() below is the only reader and it copes with null anyway, because
// applyRotation()/setTheme() are legitimately called from setup() before init() has run.
std::shared_ptr<const Config> g_cfg_ptr;
const Config &cfg() {
  return g_cfg_ptr ? *g_cfg_ptr : emptyConfig();  // config_store's one set of struct defaults
}
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
// Page change queued from off the LVGL task (POST /api/debug/page) or from a screen's own event
// handler (the tap). -1 = nothing queued. Applied by applyPendingPage(), never by the setter:
// building or deleting an lv_obj from the web task, or deleting a screen from inside its own
// event, is a crash either way.
volatile int g_pending_page = -1;
uint32_t g_page_refusals = 0;  // builds the pool could not take (GET /api/debug/ui)
uint32_t g_tick_ms_max = 0;    // worst tick() duration since boot (GET /api/debug/ui)
bool g_pool_tight = false;     // the page that is up left under kPageRuntimeHeadroom free
bool g_stalled = false;        // parked on the message screen; only a tap or a config change retries
UiDebug g_debug;  // written at the end of tick() under g_pending_mutex, read by the web task

// Guards g_debug only, since 0.3.2-rc3: the config handover no longer needs a mutex of its own
// (see g_pending). Kept under this name because debugSnapshot() and the end of tick() are the two
// sides of it and nothing else touches it.
SemaphoreHandle_t g_pending_mutex = nullptr;
// "A save has happened; pick up the new pointer." That is the WHOLE handover now - config_store
// owns the Config and publishes it, so there is nothing here to copy, nothing to move and nothing
// to lock. One volatile store from whichever task saved, one volatile read per tick, and the
// pointer comparison in tick() is what actually decides whether anything changed.
volatile bool g_pending = false;

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

void attachTapHandlers(lv_obj_t *scr) {
  lv_obj_add_event_cb(scr, onScreenPressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(scr, onScreenTapped, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// LVGL pool safety (DESIGN.md SS8 "one page at a time")
//
// LVGL allocates every widget from its own static pool - LV_MEM_SIZE in lv_conf.h, 36 KB of .bss,
// ~33 KB usable once its TLSF control block is out - and NOT from the ESP heap. `lv_free` in
// GET /api/debug/ui is what is left of that.
//
// LVGL 9.5 cannot survive lv_malloc() returning NULL in the middle of a build. lv_obj_class.c's
// create path does
//     parent->spec_attr->children = lv_realloc(...);
//     parent->spec_attr->children[child_cnt - 1] = obj;
// with no check on the realloc, so an exhausted pool is a NULL dereference; the LV_ASSERT_MALLOC
// sites that do check it hit LV_ASSERT_HANDLER instead, which this project routes through
// lv_assert_hook.cpp. Either way the device reboots in the owner's living room, on a tap. There
// is no "handle the allocation failure" answer available from outside LVGL - the only defence is
// never to start a build that cannot fit. Hence:
//
//   * exactly ONE page is resident. Every transition parks on g_parking_screen, deletes the page
//     it is leaving, and only then builds the next one. Before this rework the switch built the
//     next page first, so Main -> Stats peaked at main + night + stats all resident at once.
//   * only one of the arrivals page and the night clock exists at a time. The flip happens at
//     most a couple of times a day (when the evening's last bus goes out of range) and costs one
//     rebuild; keeping both costs the night page's share of the pool around the clock.
//   * every successful build records what it cost (g_page_cost), and a later build of the same
//     page is refused unless the pool still has that much (plus kRebuildSlack). A page is always
//     built into a pool that has just been emptied, so the first build is the best attempt that
//     will ever be made: if it does not fit then, nothing would have made it fit. What the
//     remembered cost catches is creep - a page that grows or leaks run over run - which a fixed
//     floor cannot see, and it self-calibrates per board and per stop count with no magic number.
//   * a refused build shows a readable message instead of a blank panel or a crash, logs one
//     [lvmem] line, and is counted in GET /api/debug/ui as `page_refusals`.
//
// What the check deliberately does NOT do is reserve runtime headroom on top of the build cost.
// It was written that way first and it locked the owner out of their own arrivals page: with four
// stops configured that page measures 31,656 B of a 36,864 B pool on cyd-3248S035R, so demanding
// cost + 3 KB refused a page that had been resident seconds earlier and left the device parked on
// the device info screen (measured 2026-09-16: three refusals in three cycles). A page that fits
// is built. Whether it leaves a comfortable margin afterwards is a different question, answered
// by the kPageRuntimeHeadroom warning - and by main_screen.cpp's panel guard, which is what stops
// a configuration too big for the pool from ever being built in the first place.
// ---------------------------------------------------------------------------

// The night clock gets its own accounting slot even though it shares Page::Main with the arrivals
// page: they are separately built and separately sized.
enum PageSlot { kSlotMain = 0, kSlotNight, kSlotStats, kSlotDevice, kSlotCount };
const char *const kSlotName[kSlotCount] = {"main", "night", "stats", "device"};
uint32_t g_page_cost[kSlotCount] = {0, 0, 0, 0};

// What a built page wants left over while it is up: lv_label_set_text() reallocates its text
// buffer on every change, and the ticker's lv_anim and lv_async_call records come out of the same
// pool. Measured churn across a full page cycle is ~400 B with two stops. This is a WARNING
// threshold, not a reservation - see the note above.
constexpr uint32_t kPageRuntimeHeadroom = 3 * 1024;

// Slack on the rebuild check, to absorb the difference in TLSF per-block overhead between the
// build that measured the cost and this one. Small on purpose: the check is about creep.
constexpr uint32_t kRebuildSlack = 512;

uint32_t poolFreeBytes() {
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  return m.free_size;
}

// Re-reads the palette into the parking screen (ui_common.h setTheme()); called again from
// rebuildScreens() so a theme change reaches it like every other screen.
void applyParkingStyle() {
  if (g_parking_screen == nullptr) return;
  lv_obj_set_style_bg_color(g_parking_screen, colorBg(), 0);
  lv_obj_set_style_bg_opa(g_parking_screen, LV_OPA_COVER, 0);
  if (g_parking_label == nullptr) return;
  int32_t w, h;
  screenSize(w, h);
  lv_obj_set_style_text_color(g_parking_label, colorText(), 0);
  lv_obj_set_style_text_font(g_parking_label, fontBody(h), 0);
}

// Built once, before any page, and never freed - it has to exist at the moment the pool is too
// full for anything else, which is not a moment at which to allocate. ~200 B of pool, against the
// ~20 KB a page costs. Its label is the one thing this UI can say when a page will not fit; a tap
// on it retries, and the web UI stays up so the stop list can be shortened from a phone.
void buildParkingScreen() {
  if (g_parking_screen != nullptr) return;
  int32_t w, h;
  screenSize(w, h);
  g_parking_screen = lv_obj_create(nullptr);
  lv_obj_set_size(g_parking_screen, w, h);
  lv_obj_set_style_border_width(g_parking_screen, 0, 0);
  lv_obj_set_style_pad_all(g_parking_screen, 12, 0);
  lv_obj_set_scrollable(g_parking_screen, false);
  g_parking_label = lv_label_create(g_parking_screen);
  lv_obj_set_width(g_parking_label, lv_pct(96));
  lv_obj_center(g_parking_label);
  lv_label_set_long_mode(g_parking_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(g_parking_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g_parking_label, "Not enough display memory for this page.\n\nShow fewer stops in Settings, then tap the screen.");
  lv_obj_set_hidden(g_parking_label, true);
  attachTapHandlers(g_parking_screen);
  applyParkingStyle();
}

// Deletes a page synchronously and clears the pointer. Safe ONLY when the caller is not inside
// that screen's own event handler and the screen is not the active one - which is what parking
// on g_parking_screen first guarantees. Every caller goes through parkAndDropPages().
void deletePageNow(lv_obj_t *&scr) {
  if (scr == nullptr) return;
  lv_obj_t *doomed = scr;
  scr = nullptr;  // cleared first: the LV_EVENT_DELETE handlers run inside lv_obj_delete()
  lv_obj_delete(doomed);
}

// Loads the parking screen and frees every page. After this the pool holds nothing but the
// parking screen, so whatever is built next gets the whole pool.
void parkAndDropPages() {
  if (g_parking_screen != nullptr && lv_screen_active() != g_parking_screen) {
    lv_screen_load(g_parking_screen);
  }
  deletePageNow(g_main_screen);
  deletePageNow(g_night_screen);
  deletePageNow(g_stats_screen);
  deletePageNow(g_device_info_screen);
}

// Builds one page into the (just emptied) pool and gives it its first refresh, so the cost
// recorded includes the real label text and not the placeholders create*Screen() leaves. Returns
// nullptr, having built nothing, when the pool cannot be trusted to take it.
lv_obj_t *buildSlot(PageSlot slot, const transit::Snapshot &snap) {
  uint32_t before = poolFreeBytes();
  uint32_t known = g_page_cost[slot];
  if (known != 0 && before < known + kRebuildSlack) {
    g_page_refusals++;
    Serial.printf("[lvmem] refused to build %s: %u B free, last build cost %u B\n",
                  kSlotName[slot], (unsigned)before, (unsigned)known);
    return nullptr;
  }
  lv_obj_t *scr = nullptr;
  switch (slot) {
    case kSlotMain:
      scr = createMainScreen(cfg());
      if (scr != nullptr) refreshMainScreen(scr, cfg(), snap);
      break;
    case kSlotNight:
      scr = createNightScreen(cfg());
      if (scr != nullptr) refreshNightScreen(scr, cfg(), snap);
      break;
    case kSlotStats:
      scr = createStatsScreen(cfg());
      if (scr != nullptr) refreshStatsScreen(scr);
      break;
    case kSlotDevice:
      scr = createDeviceInfoScreen(cfg());
      if (scr != nullptr) refreshDeviceInfoScreen(scr);
      break;
    default:
      break;
  }
  if (scr == nullptr) {
    g_page_refusals++;
    Serial.printf("[lvmem] %s did not build (pool free %u)\n", kSlotName[slot], (unsigned)poolFreeBytes());
    return nullptr;
  }
  attachTapHandlers(scr);
  uint32_t after = poolFreeBytes();
  uint32_t cost = before > after ? before - after : 0;
  if (cost > g_page_cost[slot]) g_page_cost[slot] = cost;
  g_pool_tight = after < kPageRuntimeHeadroom;
  Serial.printf("[lvmem] built %s: %u B, pool free %u%s\n", kSlotName[slot], (unsigned)cost,
                (unsigned)after, g_pool_tight ? "  ** tight: under 3 KB left for text updates **" : "");
  return scr;
}

// Parks and shows the one message this UI has for "the pool would not take the page". Better than
// a blank panel: the web UI is still up, so the owner can shorten the stop list from their phone.
void showPoolMessage() {
  g_stalled = true;
  if (g_parking_screen == nullptr) return;
  if (lv_screen_active() != g_parking_screen) lv_screen_load(g_parking_screen);
  if (g_parking_label != nullptr) lv_obj_set_hidden(g_parking_label, false);
}

void hidePoolMessage() {
  g_stalled = false;
  if (g_parking_label != nullptr) lv_obj_set_hidden(g_parking_label, true);
}

// Loads the arrivals page or the night clock, whichever the data calls for (Page::Main only),
// building it and freeing the other if the answer changed. Returns false when neither would fit.
bool showMainOrNight(const transit::Snapshot &snap) {
  bool night = nightConditionMet(cfg(), snap, g_shown_keys, (transit::Epoch)time(nullptr));
  if ((night ? g_night_screen : g_main_screen) == nullptr) {
    parkAndDropPages();
    lv_obj_t *built = buildSlot(night ? kSlotNight : kSlotMain, snap);
    if (built == nullptr && night) {
      // Fall back to the arrivals page: "here is what we know" beats a blank clock, and it is the
      // page the owner expects to find on the wall.
      night = false;
      built = buildSlot(kSlotMain, snap);
    }
    if (built == nullptr) return false;
    if (night) g_night_screen = built; else g_main_screen = built;
  }
  lv_obj_t *want = night ? g_night_screen : g_main_screen;
  if (lv_screen_active() != want) lv_screen_load(want);
  g_night = night;
  return true;
}

// Builds and loads whatever g_page says, assuming the pool has already been emptied for it.
bool showCurrentPage(const transit::Snapshot &snap) {
  switch (g_page) {
    case Page::Stats:
      if (g_stats_screen == nullptr) g_stats_screen = buildSlot(kSlotStats, snap);
      if (g_stats_screen == nullptr) return false;
      if (lv_screen_active() != g_stats_screen) lv_screen_load(g_stats_screen);
      return true;
    case Page::DeviceInfo:
      if (g_device_info_screen == nullptr) g_device_info_screen = buildSlot(kSlotDevice, snap);
      if (g_device_info_screen == nullptr) return false;
      if (lv_screen_active() != g_device_info_screen) lv_screen_load(g_device_info_screen);
      return true;
    case Page::Main:
    default:
      return showMainOrNight(snap);
  }
}

// The one place the visible page changes. NEVER call it from a screen's own event handler: it
// deletes the screen that event belongs to, which is a use-after-free. onScreenTapped() defers it
// through lv_async_call() and the web hook leaves it to tick(); both run after the event unwound.
void loadPage(Page target) {
  // The pointer is kept in a local so the Snapshot outlives every use of `snap` below.
  std::shared_ptr<const transit::Snapshot> snap_ptr = currentSnapshot();
  const transit::Snapshot &snap = snap_ptr ? *snap_ptr : kNoSnapshot;
  parkAndDropPages();
  hidePoolMessage();
  g_page = target;
  if (showCurrentPage(snap)) return;
  // Refused. Fall back to the ARRIVALS page, never to the page we came from. The arrivals page is
  // the product; Stats and Device info are places you visit. Falling back to wherever we happened
  // to be would let a tap strand the display on a secondary page - which is exactly what a wrong
  // headroom constant did on 2026-09-16: with four stops configured the device cycled
  // stats -> device -> (main refused) -> device for as long as it was tapped, and the arrivals
  // page, the whole point of the thing, became unreachable. A refused page returns you home.
  if (target != Page::Main) {
    Serial.println("[lvmem] page refused - going back to the arrivals page");
    g_page = Page::Main;
    if (showCurrentPage(snap)) return;
  }
  showPoolMessage();
}

// Recomputes the profile and the visible stop list. Both feed the page builders, so this runs
// before anything is built.
void refreshShownKeys() {
  g_active_profile = activeProfileIndex(cfg(), time(nullptr));
  g_shown_keys.clear();
  for (const transit::StopConfig &s : visibleStops(cfg(), time(nullptr))) g_shown_keys.push_back(s.key);
}

// DESIGN.md SS6 "quiet": backlight schedule with wake-on-touch. Returns true while dimmed.
bool applyQuietHours() {
  const QuietConfig &q = cfg().device.quiet;
  bool quiet = false;
  if (q.enabled) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    quiet = now >= 1700000000 && daypart::inWindow(lt.tm_hour * 60 + lt.tm_min, daypart::parseClock(q.start), daypart::parseClock(q.end));
  }
  bool awake = (int32_t)(millis() - g_wake_until_ms) < 0;
  bool dim = quiet && !awake;
  int target = dim ? q.brightness : cfg().device.brightness;
  if (target != g_applied_brightness) {
    applyBrightness((uint8_t)target);
    g_applied_brightness = target;
  }
  g_dimmed = dim;
  return dim;
}

// Tears down and rebuilds the visible page (rotation, theme, ticker or stop list changed). Each
// screen frees its own context struct from an LV_EVENT_DELETE handler. The remembered build costs
// are dropped with it: a config change is exactly the thing that makes a page a different size,
// and carrying a four-stop cost into a two-stop configuration would refuse builds that fit.
void rebuildScreens() {
  parkAndDropPages();
  hidePoolMessage();
  for (uint32_t &c : g_page_cost) c = 0;
  applyParkingStyle();
  refreshShownKeys();
  g_page = Page::Main;
  {
    std::shared_ptr<const transit::Snapshot> p = currentSnapshot();
    if (!showCurrentPage(p ? *p : kNoSnapshot)) showPoolMessage();
  }
}

void onScreenPressed(lv_event_t *e) {
  (void)e;
  if (g_dimmed) {
    // Wake for wake_seconds; the click that follows this press must not cycle pages.
    g_wake_until_ms = millis() + (uint32_t)cfg().device.quiet.wake_seconds * 1000u;
    g_swallow_click = true;
    applyQuietHours();
  } else if ((int32_t)(millis() - g_wake_until_ms) < 0) {
    g_wake_until_ms = millis() + (uint32_t)cfg().device.quiet.wake_seconds * 1000u;  // keep it awake
  }
}

// DESIGN.md SS8: Main -> Stats -> Device info -> Main.
Page nextPage(Page p) {
  switch (p) {
    case Page::Main: return Page::Stats;
    case Page::Stats: return Page::DeviceInfo;
    default: return Page::Main;
  }
}

// Runs the queued page change, from anywhere that is NOT inside a screen's own event handler.
void applyPendingPage() {
  int want = g_pending_page;
  if (want < 0) return;
  g_pending_page = -1;
  loadPage((Page)want);
}

void pageChangeAsyncCb(void *) {
  applyPendingPage();
}

void onScreenTapped(lv_event_t *e) {
  (void)e;
  if (g_swallow_click) {
    g_swallow_click = false;
    return;
  }
  // The transition deletes this screen, so it cannot run here - the event is still unwinding
  // through the object it would free. Queue it and let lv_async_call() run it on the next
  // lv_timer_handler() pass, which is the same few-millisecond deferral lv_obj_delete_async()
  // uses and is imperceptible on a tap. lv_async_call() takes ~24 B of the pool; if even that is
  // gone it fails, and the flag stays set for tick() to pick up within the second.
  g_pending_page = (int)nextPage(g_page);
  lv_async_call(pageChangeAsyncCb, nullptr);
}

}  // namespace

void init(std::shared_ptr<const Config> config) {
  g_cfg_ptr = std::move(config);
  g_pending = false;  // whatever we were handed IS the current config
  setTheme(cfg().device.theme);
  if (g_pending_mutex == nullptr) g_pending_mutex = xSemaphoreCreateMutex();

  buildParkingScreen();
  refreshShownKeys();
  g_page = Page::Main;
  {
    std::shared_ptr<const transit::Snapshot> p = currentSnapshot();
    if (!showCurrentPage(p ? *p : kNoSnapshot)) showPoolMessage();
  }
  g_initialized = true;
}

namespace {

// A full-screen, centred column used by both pre-init screens below. Not shared with the three
// real screens on purpose: those are rebuilt on every config change, these two exist only before
// ui::init() has run and are never rebuilt.
lv_obj_t *makeMessageScreen(int32_t w, int32_t h) {
  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_size(screen, w, h);
  lv_obj_set_style_bg_color(screen, colorBg(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 10, 0);
  lv_obj_set_style_border_width(screen, 0, 0);
  lv_obj_set_style_pad_row(screen, 6, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollable(screen, false);
  return screen;
}

lv_obj_t *makeCentredLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_width(l, lv_pct(96));
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}

}  // namespace

void showWifiSetupScreen(const std::string &ap_name, const std::string &password) {
  // Built once and reused (only the text is updated on repeat calls) rather than leaking an
  // lv_obj_t tree - and, more to the point here, a second QR canvas out of LVGL's 36 KB pool.
  if (g_wifi_setup_screen == nullptr) {
    int32_t w, h;
    screenSize(w, h);

    lv_obj_t *screen = makeMessageScreen(w, h);

    lv_obj_t *title = makeCentredLabel(screen, fontBody(h), colorText());
    lv_label_set_text(title, "Wi-Fi setup");

    g_wifi_setup_ssid_label = makeCentredLabel(screen, fontBody(h), colorEarly());

    lv_obj_t *pass_caption = makeCentredLabel(screen, fontSmall(h), colorSubtext());
    lv_label_set_text(pass_caption, "password");

    g_wifi_setup_pass_label = makeCentredLabel(screen, fontBig(h), colorText());

    // The QR carries the standard `WIFI:` join URI, which every current phone camera understands,
    // so the ten-character password never has to be typed (review F02/F10: the password only
    // exists because an open setup AP broadcasts the owner's home credentials in the clear, and a
    // password nobody can type would just get replaced by a worse one).
    //
    // Sized off the panel: a third of the shorter edge leaves room for the text above it on the
    // 240-tall boards and still gives ~3 px per QR module. LVGL allocates the canvas as a 1-bit
    // draw buffer from its own pool (~2.5 KB at this size) - affordable only because this screen
    // exists before ui::init() has built the three real ones, and the device reboots out of the
    // portal either way.
    int32_t qr_size = (w < h ? w : h) / 3;
    if (qr_size > 150) qr_size = 150;
    g_wifi_setup_qr = lv_qrcode_create(screen);
    lv_qrcode_set_size(g_wifi_setup_qr, qr_size);
    // Fixed black-on-white, not the theme colours: a QR reader needs the dark modules dark and a
    // light quiet zone around them, which an inverted dark palette would not give it.
    lv_qrcode_set_dark_color(g_wifi_setup_qr, lv_color_black());
    lv_qrcode_set_light_color(g_wifi_setup_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(g_wifi_setup_qr, true);

    lv_obj_t *hint = makeCentredLabel(screen, fontSmall(h), colorSubtext());
    lv_label_set_text(hint, "Join this network, then open http://192.168.4.1 to set up your home Wi-Fi.");

    g_wifi_setup_screen = screen;
  }

  lv_label_set_text(g_wifi_setup_ssid_label, ap_name.c_str());
  lv_label_set_text(g_wifi_setup_pass_label, password.c_str());
  if (g_wifi_setup_qr != nullptr) {
    // WIFI:T:WPA;S:<ssid>;P:<pass>;; - the de-facto standard both Android and iOS cameras read.
    // No escaping pass here: the SSID is "TransitDisplay-XXXX" and the password comes from
    // auth.cpp's alphanumeric alphabet, so neither can contain the \ ; , : " that the format
    // would need escaped. If either ever becomes user-supplied, escape them first.
    std::string payload = "WIFI:T:WPA;S:" + ap_name + ";P:" + password + ";;";
    lv_qrcode_update(g_wifi_setup_qr, payload.c_str(), (uint32_t)payload.size());
  }
  lv_screen_load(g_wifi_setup_screen);
}

void showConnectingScreen(const std::string &ssid, const std::string &detail) {
  if (g_connecting_screen == nullptr) {
    int32_t w, h;
    screenSize(w, h);

    lv_obj_t *screen = makeMessageScreen(w, h);
    // The whole screen is the tap target: this is the only way into the setup portal on a device
    // that already has credentials (review F10), and the owner should not have to find a button.
    lv_obj_set_clickable(screen, true);
    lv_obj_add_event_cb(screen, [](lv_event_t *) { g_tap_requested = true; }, LV_EVENT_CLICKED, nullptr);

    g_connecting_ssid_label = makeCentredLabel(screen, fontBody(h), colorText());
    g_connecting_detail_label = makeCentredLabel(screen, fontSmall(h), colorSubtext());

    lv_obj_t *hint = makeCentredLabel(screen, fontSmall(h), colorEarly());
    lv_label_set_text(hint, "Tap the screen to open Wi-Fi setup instead");

    g_connecting_screen = screen;
  }

  lv_label_set_text_fmt(g_connecting_ssid_label, "Connecting to %s...", ssid.c_str());
  lv_label_set_text(g_connecting_detail_label, detail.c_str());
  if (lv_screen_active() != g_connecting_screen) lv_screen_load(g_connecting_screen);
}

void tick() {
  if (!g_initialized) {
    return;
  }
  // How long this whole refresh took, and the worst since boot (GET /api/debug/ui). The central
  // policy in ui_lock.h is what KEEPS the display task from blocking; this is what would make it
  // obvious if something ever slipped past it. A tick is tens of milliseconds - a page build is the
  // expensive one - so a reading in the hundreds means this task waited for something, and the only
  // things it can wait for are the locks it is not allowed to wait for. Two 32-bit words and a
  // subtraction.
  const uint32_t tick_start_ms = millis();
  // Pick up a configuration saved on another task (rotation, brightness, stops).
  //
  // THERE IS NO COPY LEFT IN THIS PATH. It used to be two: onConfigChanged() copy-assigned a whole
  // Config into g_pending_cfg on the web task, and this moved it into a second resident Config
  // here - so a save left ~1.2 KB of small string blocks in new places twice over, for a value the
  // device already had (runtime audit rec #8). config_store now owns the one Config and publishes
  // it by pointer; what happens here is a refcount bump, which cannot allocate and cannot throw.
  //
  // activeConfigPtr() takes its lock under the zero-tick rule on this task and keeps a LastGood of
  // the pointer (ui_lock.h, config_store.cpp). A miss returns the pointer we are already holding,
  // so `next != g_cfg_ptr` is false and the flag is put back for the next tick. That is the same
  // "a miss costs one frame" contract every other accessor has - and it is why the test is on the
  // POINTER and not on whether the call succeeded, which a miss makes indistinguishable.
  //
  // THE FLAG IS CLEARED BEFORE THE READ, NOT AFTER, and the order is the whole correctness of the
  // handover. Clearing afterwards loses a save to this interleaving: this task reads the pointer
  // (still A), the web task then publishes B and sets the flag, and this task then clears it -
  // leaving the store on B, the screens on A, and nothing to say so until the next save. Clearing
  // first inverts that: a publish landing any time after the clear re-arms the flag, and the worst
  // case is one redundant read next tick.
  if (g_pending) {
    g_pending = false;
    std::shared_ptr<const Config> next = activeConfigPtr();
    if (!next || next == g_cfg_ptr) {
      // A lock miss, almost always - config_store publishes a fresh pointer on every save, so
      // "unchanged" here means the read did not get through. Look again next tick; uiLockMisses()
      // on GET /api/debug/ui is what says how often that happens.
      g_pending = true;
    } else {
      const bool rotate = next->device.rotation != cfg().device.rotation;
      const bool invert = next->device.invert_colors != cfg().device.invert_colors;
      // The outgoing Config is released by this assignment - nothing else holds it once the store
      // has published past it - and that free happens here, on this task, outside every lock.
      g_cfg_ptr = std::move(next);
      setTheme(cfg().device.theme);  // rebuildScreens() below re-reads every colour
      if (rotate) applyRotation(cfg().device.rotation);
      if (invert) applyInvert(cfg().device.invert_colors);
      g_applied_brightness = -1;  // applyQuietHours() below re-applies whichever brightness applies
      rebuildScreens();
    }
  }

  // Commute profiles (profiles.h): the visible stop list changed -> rebuild the pages.
  int profile = activeProfileIndex(cfg(), time(nullptr));
  if (profile != g_active_profile) {
    g_active_profile = profile;
    rebuildScreens();
  }

  if (g_tap_requested) {  // POST /api/debug/tap: the same two events a finger produces
    g_tap_requested = false;
    onScreenPressed(nullptr);
    onScreenTapped(nullptr);
  }
  // POST /api/debug/page, and the tap above if lv_async_call() could not take it. We are on the
  // LVGL task and not inside any screen's event handler, so the transition is safe here.
  applyPendingPage();

  bool dimmed = applyQuietHours();
  // The pointer is held for the whole tick so the Snapshot cannot be freed under the screens while
  // they read it; the poller may publish a new one meanwhile, and this frame simply finishes with
  // the one it started on.
  std::shared_ptr<const transit::Snapshot> snap_ptr = currentSnapshot();
  const transit::Snapshot &snap = snap_ptr ? *snap_ptr : kNoSnapshot;
  g_due_active = dueAlertTick(cfg(), snap, g_shown_keys, dimmed, (transit::Epoch)time(nullptr));

  // Parked on the message screen: a build was refused and nothing has changed since. Retrying it
  // every second would just be the same refusal (or, for a page never built, the same crash) at
  // 1 Hz. A tap or a config change clears it.
  if (!g_stalled) {
    switch (g_page) {
      case Page::Main:
        // Also flips between the arrivals page and the night clock, rebuilding whichever the data
        // now calls for - only one of the two is ever resident.
        if (!showMainOrNight(snap)) {
          showPoolMessage();
        } else if (g_night) {
          refreshNightScreen(g_night_screen, cfg(), snap);
        } else {
          refreshMainScreen(g_main_screen, cfg(), snap);
        }
        break;
      case Page::DeviceInfo:
        if (g_device_info_screen != nullptr) refreshDeviceInfoScreen(g_device_info_screen);
        break;
      case Page::Stats:
        // Stats are a 30-day rollup (DESIGN.md SS8); no need to re-stream the SD card at the same
        // ~1Hz cadence as the live arrivals screen. getStopSummary() answers from a cache the poller refreshes (10 min, never blocking this task)
        // (net_poller.cpp), so this just re-reads that cache while the page is visible.
        if (g_stats_screen != nullptr) refreshStatsScreen(g_stats_screen);
        break;
    }
  }

  UiDebug d;
  d.page = g_stalled ? "stalled"
           : g_page == Page::Stats ? "stats"
           : g_page == Page::DeviceInfo ? "device"
                                        : (g_night ? "night" : "main");
  d.dimmed = dimmed;
  d.brightness = g_applied_brightness;
  d.due_active = g_due_active;
  d.chimes = dueChimesPlayed();
  d.active_profile = g_active_profile >= 0 && (size_t)g_active_profile < cfg().profiles.size() ? cfg().profiles[(size_t)g_active_profile].name : "";
  d.shown_stops = g_shown_keys;
  if (g_main_screen != nullptr) {
    mainScreenDebug(g_main_screen, d.hidden_panels, d.ticker, d.rows);
  }
  d.header_weather = cfg().weather.enabled ? headerWeatherText() : "";
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  d.lv_used = m.total_size - m.free_size;
  d.lv_free = m.free_size;
  d.lv_max_used = m.max_used;
  // LV_MEM_SIZE, not lv_mem_monitor's total_size: the monitor walks the pool and sums the BLOCKS
  // it finds, so its total shrinks as fragmentation adds per-block TLSF headers (33,264 on a
  // freshly built arrivals page, 34,416 a page change later - a moving ceiling is useless to
  // compare a high-water mark against). lv_used + lv_free therefore falls a little short of
  // lv_total, and that difference is TLSF's own overhead.
  d.lv_total = LV_MEM_SIZE;
  d.lv_frag_pct = m.frag_pct;
  d.page_refusals = g_page_refusals;
  d.pool_tight = g_pool_tight;
  d.lock_misses = uiLockMisses();
  d.tick_ms = millis() - tick_start_ms;
  if (d.tick_ms > g_tick_ms_max) g_tick_ms_max = d.tick_ms;
  d.tick_ms_max = g_tick_ms_max;
  for (int i = 0; i < kSlotCount; i++) d.page_cost[i] = g_page_cost[i];
  lv_display_t *disp = lv_display_get_default();
  d.hor_res = lv_display_get_horizontal_resolution(disp);
  d.ver_res = lv_display_get_vertical_resolution(disp);
  // Same policy: a miss leaves GET /api/debug/ui reading the previous tick's snapshot of the UI,
  // which is a 1 Hz sample of a 1 Hz value.
  {
    SharedLock lk(g_pending_mutex, 20);  // g_debug = d copies five strings and two string vectors
    if (lk) g_debug = d;
  }
}

UiDebug debugSnapshot() {
  UiDebug d;
  // 500 ms, not 200: this is how the device suite watches the panel, including while the UI task
  // is mid-rebuild and holding core 1, and a timeout here answers a default-constructed UiDebug -
  // an all-zero pool reading that looks like data. Seen intermittently at 200 ms on 2026-09-16.
  SharedLock lk(g_pending_mutex, SharedLock::RawWait{500});  // the copy below can throw
  if (lk) d = g_debug;
  return d;
}

void requestTap() {
  g_tap_requested = true;
}

bool requestPage(const std::string &name) {
  // Queued exactly like requestTap(): a volatile int the LVGL task picks up in tick(). Nothing
  // here touches an lv_obj - building or freeing one from the web server task is a crash, and
  // the whole point of this hook is to exercise the pool safely.
  int want;
  if (name == "main" || name == "night") {
    want = (int)Page::Main;  // "night" is Page::Main; the data decides which of the two is built
  } else if (name == "stats") {
    want = (int)Page::Stats;
  } else if (name == "device") {
    want = (int)Page::DeviceInfo;
  } else {
    return false;
  }
  g_pending_page = want;
  return true;
}

bool consumeTap() {
  if (!g_tap_requested) return false;
  g_tap_requested = false;
  return true;
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

void onConfigChanged() {
  // One volatile store, from whichever task just saved. No lock, no copy, nothing that can throw
  // and nothing that can fail - which also removes the old failure mode, where a 200 ms wait that
  // timed out dropped the save silently and the screen simply never noticed it. config_store has
  // already published the new Config by the time this is called (web_server.cpp), so the next
  // tick() reads a different pointer and applies it.
  g_pending = true;
}

void logMemory() {
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  Serial.printf("[lvmem] total=%u free=%u used_pct=%u frag_pct=%u max_used=%u\n", (unsigned)m.total_size, (unsigned)m.free_size, (unsigned)m.used_pct, (unsigned)m.frag_pct, (unsigned)m.max_used);
}

}  // namespace transit_app::ui
