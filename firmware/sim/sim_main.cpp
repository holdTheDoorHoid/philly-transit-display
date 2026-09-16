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
  lv_obj_t *main = ui::createMainScreen(cfg);
  lv_obj_t *stats = ui::createStatsScreen(cfg);
  lv_obj_t *device = ui::createDeviceInfoScreen(cfg);
  lv_obj_t *night = with_night ? ui::createNightScreen(cfg) : nullptr;
  std::printf("[%s %s%s] lv_used after build = %u bytes (host pointers: an over-estimate of the ESP32)\n",
              resName(c).c_str(), cfg.device.theme.c_str(), suffix.c_str(), (unsigned)lvUsed());

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

// Disconnected Wi-Fi on the device page (0 bars, no IP).
void renderOffline(Job &job, Canvas &c, const Config &cfg) {
  sim::wifi_connected = false;
  lv_obj_t *device = ui::createDeviceInfoScreen(cfg);
  ui::refreshDeviceInfoScreen(device);
  job.shot(c, device, "device-" + resName(c) + "-" + cfg.device.theme + "-offline");
  lv_screen_load(c.blank);
  lv_obj_delete(device);
  sim::wifi_connected = true;
}

struct Res {
  int32_t w, h;
};

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <out_dir> [full]\n", argv[0]);
    return 2;
  }
  Job job;
  job.out_dir = argv[1];
  bool full = argc >= 3 && std::strcmp(argv[2], "full") == 0;
  mkdir(job.out_dir.c_str(), 0755);

  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);  // the default device.tz: the clock reads as Philly time
  tzset();

  lv_init();
  lv_tick_set_cb(tickCb);

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
