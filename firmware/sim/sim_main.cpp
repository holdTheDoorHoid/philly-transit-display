// Screenshot simulator for the LVGL screens (firmware/sim/README.md).
//
// Builds the real main/stats/device-info/night screens from src/app/ui/ against a host LVGL
// display whose draw buffer is a whole RGB565 frame, refreshes them the way ui.cpp's tick() does,
// and writes each frame out as a PNG. The point is to be able to LOOK at a layout change on every
// board size and both themes without a panel in front of you (DESIGN.md SS8 lays out from the
// runtime resolution, so 320x480, 480x320, 240x320 and 320x240 all differ).
//
//   .pio/build/ui-sim/program <out_dir> [full]
//
// The default set is the owner's board (320x480 portrait, and 480x320 rotated) plus the 2.4"/2.8"
// boards landscape (320x240), every page, light and dark. `full` adds 240x320 portrait, the
// four-stop configuration that stresses the small boards, and the stale-header / no-stats cases.
#include <lvgl.h>
#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "app/config_store.h"
#include "app/demo_data.h"
#include "app/ui/device_info_screen.h"
#include "app/ui/main_screen.h"
#include "app/ui/night_screen.h"
#include "app/ui/stats_screen.h"
#include "app/ui/ui_common.h"
#include "sim_fakes.h"
#include "stubs/Arduino.h"
#include "stubs/WiFi.h"

using transit_app::Config;
namespace ui = transit_app::ui;

namespace transit_app::ui {
extern bool g_sim_small_board;  // ui_common.cpp: pick the 240-tall boards' Montserrat 20 for the big minutes
}

namespace {

uint32_t g_tick_ms = 0;
uint32_t tickCb() {
  return g_tick_ms;
}
void flushCb(lv_display_t *d, const lv_area_t *, uint8_t *) {
  lv_display_flush_ready(d);  // RENDER_MODE_DIRECT: the buffer already is the frame
}

struct Canvas {
  int32_t w = 0, h = 0;
  std::vector<uint16_t> px;
  lv_display_t *disp = nullptr;
  lv_obj_t *blank = nullptr;  // loaded before a screen is deleted: LVGL will not delete the active one
};

Canvas openDisplay(int32_t w, int32_t h) {
  Canvas c;
  c.w = w;
  c.h = h;
  c.px.assign((size_t)w * (size_t)h, 0);
  c.disp = lv_display_create(w, h);
  lv_display_set_color_format(c.disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(c.disp, c.px.data(), nullptr, (uint32_t)(c.px.size() * sizeof(uint16_t)),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(c.disp, flushCb);
  lv_display_set_default(c.disp);
  c.blank = lv_obj_create(nullptr);
  return c;
}

void closeDisplay(Canvas &c) {
  lv_screen_load(c.blank);
  lv_display_delete(c.disp);  // deletes every screen on it, including blank
  c.disp = nullptr;
}

// ---- PNG output (zlib for deflate + crc32) ----
void putU32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back((uint8_t)(x >> 24));
  v.push_back((uint8_t)(x >> 16));
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}

void writeChunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> len;
  putU32(len, (uint32_t)data.size());
  fwrite(len.data(), 1, 4, f);
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  fwrite(body.data(), 1, body.size(), f);
  std::vector<uint8_t> crc;
  putU32(crc, (uint32_t)crc32(0L, body.data(), (uInt)body.size()));
  fwrite(crc.data(), 1, 4, f);
}

bool writePng(const std::string &path, const Canvas &c) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t)c.h * (1 + (size_t)c.w * 3));
  for (int32_t y = 0; y < c.h; ++y) {
    raw.push_back(0);  // filter: none
    for (int32_t x = 0; x < c.w; ++x) {
      uint16_t p = c.px[(size_t)y * c.w + x];  // lv_color16_t: red in the top 5 bits
      raw.push_back((uint8_t)(((p >> 11) & 31) * 255 / 31));
      raw.push_back((uint8_t)(((p >> 5) & 63) * 255 / 63));
      raw.push_back((uint8_t)((p & 31) * 255 / 31));
    }
  }
  uLongf clen = compressBound((uLong)raw.size());
  std::vector<uint8_t> comp(clen);
  if (compress2(comp.data(), &clen, raw.data(), (uLong)raw.size(), 9) != Z_OK) return false;
  comp.resize(clen);

  FILE *f = fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  fwrite(kSig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  putU32(ihdr, (uint32_t)c.w);
  putU32(ihdr, (uint32_t)c.h);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(2);  // colour type: RGB
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  writeChunk(f, "IHDR", ihdr);
  writeChunk(f, "IDAT", comp);
  writeChunk(f, "IEND", {});
  fclose(f);
  return true;
}

// Runs a few LVGL frames (timers, animations) and renders the active screen into the buffer.
void settle(Canvas &c, int frames = 6) {
  for (int i = 0; i < frames; ++i) {
    g_tick_ms += 33;
    sim::millis_ms += 33;
    lv_timer_handler();
  }
  lv_refr_now(c.disp);
}

uint32_t countObjs(lv_obj_t *o) {
  uint32_t n = 1;
  for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) n += countObjs(lv_obj_get_child(o, i));
  return n;
}

uint32_t lvUsed() {
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  return m.total_size - m.free_size;
}

// The pool's free bytes. This - not lvUsed() - is what the pool sweep measures deltas of, because
// it is the metric ui.cpp's buildSlot() records on the device (and what GET /api/debug/ui reports
// as lv_free), so the two are directly comparable. lv_mem_monitor's total_size is the sum of the
// BLOCKS it walks, so it moves with fragmentation and a used-delta taken from it is not a byte
// count of anything.
uint32_t lvFree() {
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  return m.free_size;
}

// ---- Configurations ----
transit::StopConfig busStop(const char *key, const char *route, const char *stop_id, const char *direction,
                            const char *headsign, const char *label, const char *stop_name) {
  transit::StopConfig s;
  s.key = key;
  s.mode = transit::Mode::Bus;
  s.route = route;
  s.stop_id = stop_id;
  s.direction = direction;
  s.headsign = headsign;
  s.label = label;
  s.stop_name = stop_name;
  s.show = 3;
  s.lat = 39.93;
  s.lng = -75.17;
  return s;
}

// The owner's two Route 17 stops (DESIGN.md SS1), crowding words + icons, two Indego stations.
Config ownerConfig(const std::string &theme) {
  Config c;
  c.device.theme = theme;
  c.device.crowding = "both";
  c.stops = {busStop("17-21332", "17", "21332", "1", "20th-Johnston", "17 Southbound", "19th St & Mifflin St"),
             busStop("17-21297", "17", "21297", "0", "2nd-Market", "17 Northbound", "20th St & Mifflin St")};
  c.bike.enabled = true;
  c.bike.stations = {{3005, "Snyder & Dorrance"}, {3121, "18th & Fernon, Aquinas Center"}};
  return c;
}

// Four stops: the case that has to fit on the 320x240 boards without clipping.
Config fourStopConfig(const std::string &theme) {
  Config c = ownerConfig(theme);
  c.stops.push_back(busStop("34-20563", "34", "20563", "0", "61st-Baltimore", "34 Westbound", "13th & Market"));
  c.stops.back().mode = transit::Mode::Trolley;
  transit::StopConfig bsl = busStop("BSL-1286", "BSL", "1286", "1", "NRG", "BSL Southbound", "Tasker-Morris");
  bsl.mode = transit::Mode::Subway;
  c.stops.push_back(bsl);
  c.bike.enabled = false;
  return c;
}

void ownerSummaries() {
  sim::clearSummaries();
  sim::setSummary("17-21332", sim::summary(87.0f, 1.3f, 17, 3, 123, 40));
  sim::setSummary("17-21297", sim::summary(71.0f, -0.4f, 8, 5, 98, 31));
}

void fourStopSummaries() {
  sim::clearSummaries();
  sim::setSummary("17-21332", sim::summary(87.0f, 1.3f, 17, 3, 123, 40));
  sim::setSummary("17-21297", sim::summary(58.0f, 7.2f, 17, 11, 210, 66));
  // "34-20563" left unset: nothing cached yet -> "loading"
  sim::setSummary("BSL-1286", sim::summary(-1.0f, 0.0f, -1, 0, 0, 0));  // samples 0 -> "no data yet"
}

void noOnTimeSummaries() {
  sim::clearSummaries();
  sim::setSummary("17-21332", sim::summary(-1.0f, 0.0f, 14, 2, 12, 12));  // samples, none with a known lateness
  sim::setSummary("17-21297", sim::summary(-1.0f, 0.0f, -1, 0, 0, 0));
}

// ---- Rendering ----
struct Job {
  std::string out_dir;
  bool ok = true;
  void shot(Canvas &c, lv_obj_t *scr, const std::string &name) {
    lv_screen_load(scr);
    settle(c);
    std::string path = out_dir + "/" + name + ".png";
    if (!writePng(path, c)) {
      std::printf("FAILED to write %s\n", path.c_str());
      ok = false;
    } else {
      std::printf("wrote %-48s objects=%u\n", path.c_str(), (unsigned)countObjs(scr));
    }
  }
};

std::string resName(const Canvas &c) {
  return std::to_string(c.w) + "x" + std::to_string(c.h);
}

// One resolution, one theme: the four pages the device cycles through, from `cfg`.
void renderPages(Job &job, Canvas &c, const Config &cfg, const std::string &suffix, bool with_night) {
  transit::Snapshot snap = transit_app::buildDemoSnapshot((transit::Epoch)time(nullptr));
  // Pool cost per screen (host pointers: an over-estimate of the ESP32 by roughly 1/0.65). On the
  // device only main + night are resident; stats and device are built when tapped to (ui.cpp).
  uint32_t before = lvUsed();
  lv_obj_t *main = ui::createMainScreen(cfg);
  uint32_t after_main = lvUsed();
  lv_obj_t *stats = ui::createStatsScreen(cfg);
  uint32_t after_stats = lvUsed();
  lv_obj_t *device = ui::createDeviceInfoScreen(cfg);
  uint32_t after_device = lvUsed();
  lv_obj_t *night = with_night ? ui::createNightScreen(cfg) : nullptr;
  uint32_t after_night = lvUsed();
  std::printf("[%s %s%s] lv pool, host bytes: main %u, stats %u, device %u, night %u\n", resName(c).c_str(),
              cfg.device.theme.c_str(), suffix.c_str(), (unsigned)(after_main - before),
              (unsigned)(after_stats - after_main), (unsigned)(after_device - after_stats),
              (unsigned)(after_night - after_device));

  ui::refreshMainScreen(main, cfg, snap);
  job.shot(c, main, "main-" + resName(c) + "-" + cfg.device.theme + suffix);
  ui::refreshStatsScreen(stats);
  job.shot(c, stats, "stats-" + resName(c) + "-" + cfg.device.theme + suffix);
  ui::refreshDeviceInfoScreen(device);
  job.shot(c, device, "device-" + resName(c) + "-" + cfg.device.theme + suffix);
  if (night) {
    ui::refreshNightScreen(night, cfg, snap);
    job.shot(c, night, "night-" + resName(c) + "-" + cfg.device.theme + suffix);
  }

  lv_screen_load(c.blank);
  lv_obj_delete(main);
  lv_obj_delete(stats);
  lv_obj_delete(device);
  if (night) lv_obj_delete(night);
}

// The header's stale state: the snapshot is five minutes old and Wi-Fi is down to one bar.
void renderStale(Job &job, Canvas &c, const Config &cfg) {
  transit::Snapshot snap = transit_app::buildDemoSnapshot((transit::Epoch)time(nullptr));
  snap.generated = (transit::Epoch)time(nullptr) - 300;
  int rssi = sim::wifi_rssi;
  sim::wifi_rssi = -80;
  lv_obj_t *main = ui::createMainScreen(cfg);
  ui::refreshMainScreen(main, cfg, snap);
  job.shot(c, main, "main-" + resName(c) + "-" + cfg.device.theme + "-stale");
  lv_screen_load(c.blank);
  lv_obj_delete(main);
  sim::wifi_rssi = rssi;
}

// Everything wrong at once on the device page: Wi-Fi down (0 bars, no IP), the last SEPTA poll
// failed, the weather and alerts stale, no Indego fetch yet, and the SD card dropping rows.
void renderOffline(Job &job, Canvas &c, const Config &cfg) {
  sim::wifi_connected = false;
  sim::poll_ok = false;
  sim::poll_age_s = 4 * 60;
  sim::poll_error = "connect failed";
  sim::weather_age_s = 2 * 3600;
  sim::bike_age_s = -1;
  sim::alerts_age_s = 20 * 60;
  sim::sd_dropped_rows = 3;
  sim::sd_error = "write failed";
  lv_obj_t *device = ui::createDeviceInfoScreen(cfg);
  ui::refreshDeviceInfoScreen(device);
  job.shot(c, device, "device-" + resName(c) + "-" + cfg.device.theme + "-offline");
  lv_screen_load(c.blank);
  lv_obj_delete(device);
  sim::wifi_connected = true;
  sim::poll_ok = true;
  sim::poll_age_s = 12;
  sim::poll_error.clear();
  sim::weather_age_s = 4 * 60;
  sim::bike_age_s = 90;
  sim::alerts_age_s = 3 * 60;
  sim::sd_dropped_rows = 0;
  sim::sd_error.clear();
}

struct Res {
  int32_t w, h;
};

// ---- Pool sweep (`program <out_dir> pool`) ----
//
// What it answers: how the LVGL pool cost of each page grows with the stop count, on each of the
// four panel sizes, with every page built into an EMPTY pool exactly as ui.cpp now builds them.
//
// What it cannot answer: the absolute number of bytes on the board. The host build has 64-bit
// pointers and a 512 KB LV_MEM_SIZE (platformio.ini), and lv_obj is pointer-heavy, so every figure
// here is larger than the ESP32's.
//
// kHostToBoard is that gap, and it is not a guess - it was fitted against six figures measured on
// cyd-3248S035R on 2026-09-16 through GET /api/debug/ui's `lv_page_cost`, which records the same
// free-bytes delta this sweep does:
//
//   page    stops   board B   host B   ratio
//   main      2      19,280   29,240   0.6594
//   main      4      31,656   47,864   0.6614
//   stats     2       8,220   12,472   0.6591
//   stats     4      12,860   19,896   0.6464
//   device    2      10,336   15,816   0.6535
//   device    4      10,288   15,792   0.6515
//
// Spread 0.646-0.661 across two page shapes and two stop counts, so 0.66 predicts this board to
// about 1.5 %. It has NOT been checked on the 240-tall boards; those rows are the same arithmetic
// applied to a build nobody has measured, and should be read as "roughly this" until someone does.
constexpr double kHostToBoard = 0.66;
constexpr uint32_t kBoardPool = 36 * 1024;  // LV_MEM_SIZE on every board env (lv_conf.h)

Config stopsConfig(const std::string &theme, int n) {
  Config c = ownerConfig(theme);
  c.device.crowding = "icons";  // the owner's setting, so the sweep calibrates against their board
  static const char *const kExtra[][7] = {
      {"17-10255", "17", "10255", "0", "2nd-Market", "17 Northbound", "Market St & 10th St"},
      {"17-10258", "17", "10258", "1", "20th-Johnston", "17 Southbound", "Market St & 11th St"},
      {"17-10262", "17", "10262", "0", "2nd-Market", "17 Northbound", "Market St & 12th St"},
      {"17-10266", "17", "10266", "1", "20th-Johnston", "17 Southbound", "Market St & 13th St"},
      {"17-10270", "17", "10270", "0", "2nd-Market", "17 Northbound", "Market St & 15th St"},
      {"17-10274", "17", "10274", "1", "20th-Johnston", "17 Southbound", "Market St & 16th St"},
  };
  for (int i = 0; (int)c.stops.size() < n && i < (int)(sizeof(kExtra) / sizeof(kExtra[0])); ++i) {
    c.stops.push_back(busStop(kExtra[i][0], kExtra[i][1], kExtra[i][2], kExtra[i][3], kExtra[i][4],
                              kExtra[i][5], kExtra[i][6]));
  }
  return c;
}

// Builds one page into an empty pool, refreshes it (so the cost includes the real label text, not
// the placeholders create*Screen() leaves), measures, and frees it again.
uint32_t g_last_objects = 0;   // objects in the page pageCost() last built
uint32_t g_last_free = 0;      // pool free with that page up

uint32_t pageCost(const Config &cfg, const transit::Snapshot &snap, Canvas &c, const char *which) {
  uint32_t before = lvFree();
  lv_obj_t *scr = nullptr;
  if (std::strcmp(which, "main") == 0) {
    scr = ui::createMainScreen(cfg);
    ui::refreshMainScreen(scr, cfg, snap);
  } else if (std::strcmp(which, "night") == 0) {
    scr = ui::createNightScreen(cfg);
    ui::refreshNightScreen(scr, cfg, snap);
  } else if (std::strcmp(which, "stats") == 0) {
    scr = ui::createStatsScreen(cfg);
    ui::refreshStatsScreen(scr);
  } else {
    scr = ui::createDeviceInfoScreen(cfg);
    ui::refreshDeviceInfoScreen(scr);
  }
  settle(c, 2);
  uint32_t after = lvFree();
  uint32_t cost = before > after ? before - after : 0;
  g_last_objects = countObjs(scr);
  g_last_free = after;
  lv_screen_load(c.blank);
  lv_obj_delete(scr);
  return cost;
}

void poolSweep() {
  static const Res kAll[] = {{320, 480}, {480, 320}, {320, 240}, {240, 320}};
  std::printf("\nLVGL pool cost per page. Host bytes, and x%.2f for the board (see kHostToBoard).\n",
              kHostToBoard);
  std::printf("\"1 page\" is what this design's worst moment costs - only the largest page is ever\n"
              "resident. \"all 4\" is what the switch cost before the rework, when Stats -> Device\n"
              "held main + night + stats + device at once. Board pool is %u B.\n\n", (unsigned)kBoardPool);
  std::printf("This build's pool is %u B = %.0f board-equivalent bytes.\n\n", (unsigned)LV_MEM_SIZE,
              LV_MEM_SIZE * kHostToBoard);
  std::printf("%-9s %5s  %7s %7s %7s %7s   %8s %5s   %8s %5s  %5s %8s\n", "board", "stops", "main",
              "night", "stats", "device", "1 page", "fits", "all 4", "fits", "objs", "free");
  for (const Res &r : kAll) {
    transit_app::ui::g_sim_small_board = (r.h <= 240 || (r.w == 240 && r.h == 320));
    Canvas c = openDisplay(r.w, r.h);
    for (int n = 2; n <= 8; ++n) {
      Config cfg = stopsConfig("dark", n);
      ownerSummaries();
      transit::Snapshot snap = transit_app::buildDemoSnapshot((transit::Epoch)time(nullptr));
      uint32_t m = pageCost(cfg, snap, c, "main");
      uint32_t main_objs = g_last_objects, main_free = g_last_free;
      uint32_t ni = pageCost(cfg, snap, c, "night");
      uint32_t st = pageCost(cfg, snap, c, "stats");
      uint32_t dv = pageCost(cfg, snap, c, "device");
      // What this rework costs at its worst: the largest single page. What the old switch cost at
      // its worst: main + night + stats + device, which is what Stats -> Device held at once.
      uint32_t one = m;
      if (ni > one) one = ni;
      if (st > one) one = st;
      if (dv > one) one = dv;
      uint32_t old_peak = m + ni + st + dv;
      double one_b = one * kHostToBoard, old_b = old_peak * kHostToBoard;
      // `objs` is the arrivals page's object count: when it stops growing with the stop count,
      // main_screen.cpp's panel guard has started leaving panels off. `free` is what the arrivals
      // page leaves in the pool, in board-equivalent bytes.
      std::printf("%-9s %5d  %7.0f %7.0f %7.0f %7.0f   %8.0f %5s   %8.0f %5s  %5u %8.0f\n",
                  (std::to_string(r.w) + "x" + std::to_string(r.h)).c_str(), n, m * kHostToBoard,
                  ni * kHostToBoard, st * kHostToBoard, dv * kHostToBoard, one_b,
                  one_b < kBoardPool ? "yes" : "NO", old_b, old_b < kBoardPool ? "yes" : "NO",
                  (unsigned)main_objs, main_free * kHostToBoard);
    }
    closeDisplay(c);
  }
  std::printf("\nAll figures are board bytes (host bytes x %.2f). The arrivals page grows by about\n"
              "6.2 KB per stop on a 320-wide/480-tall panel and 4.4 KB on a 240-tall one, so five\n"
              "stops does not fit the 36 KB pool on the bigger boards however the pages are ordered -\n"
              "which is what main_screen.cpp's panel guard is for.\n", kHostToBoard);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <out_dir> [full|pool]\n", argv[0]);
    return 2;
  }
  Job job;
  job.out_dir = argv[1];
  const char *mode = argc >= 3 ? argv[2] : "";
  bool full = std::strcmp(mode, "full") == 0;
  bool pool_only = std::strcmp(mode, "pool") == 0;
  if (!pool_only) mkdir(job.out_dir.c_str(), 0755);

  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);  // the default device.tz: the clock reads as Philly time
  tzset();

  lv_init();
  lv_tick_set_cb(tickCb);

  if (pool_only) {
    ui::setTheme("dark");
    poolSweep();
    std::printf("lv_used after teardown = %u bytes (should be back near zero)\n", (unsigned)lvUsed());
    return 0;
  }

  std::vector<Res> resolutions = {{320, 480}, {480, 320}, {320, 240}};
  if (full) resolutions.push_back({240, 320});

  for (const char *theme : {"light", "dark"}) {
    for (const Res &r : resolutions) {
      ui::setTheme(theme);
      // The 2.4"/2.8" boards are 240 px on the short edge and use Montserrat 20 for the big minutes
      // (ui_common.cpp fontBig); the 3.5" board uses 28.
      ui::g_sim_small_board = (r.w < 320 || r.h < 320);
      Canvas c = openDisplay(r.w, r.h);

      ownerSummaries();
      renderPages(job, c, ownerConfig(theme), "", true);
      if (full) {
        fourStopSummaries();
        renderPages(job, c, fourStopConfig(theme), "-4stops", false);
      }
      if (full && std::string(theme) == "light") {
        ownerSummaries();
        renderStale(job, c, ownerConfig(theme));
        renderOffline(job, c, ownerConfig(theme));
        noOnTimeSummaries();
        lv_obj_t *stats = ui::createStatsScreen(ownerConfig(theme));
        ui::refreshStatsScreen(stats);
        job.shot(c, stats, "stats-" + resName(c) + "-" + theme + "-noontime");
        lv_screen_load(c.blank);
        lv_obj_delete(stats);
      }

      closeDisplay(c);
    }
  }
  std::printf("lv_used after teardown = %u bytes (should be back near zero)\n", (unsigned)lvUsed());
  return job.ok ? 0 : 1;
}
