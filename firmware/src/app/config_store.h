// LittleFS <-> Config struct: load, save, defaults, validation.
// DESIGN.md SS6 is the schema; transit_core/model.h's transit::StopConfig is
// the authoritative per-stop struct (this file does not redefine it).
#pragma once
#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "transit_core/model.h"

namespace transit_app {

constexpr const char *kConfigPath = "/config.json";
constexpr size_t kMaxStops = 8;  // DESIGN.md SS6: "Maximum 8 stops."

// Board build flag (firmware/boards/*.json): whether this board's panel shows correct colours
// only with the controller's inversion command on. Off for every vendored board (the owner's
// 3.5" panel is not inverted - an earlier "white background" was LVGL painting nothing, see
// main_screen.cpp makeBox()). Runtime-overridable via device.invert_colors.
#ifndef DISPLAY_INVERT_DEFAULT
#define DISPLAY_INVERT_DEFAULT 0
#endif

// Which items the main screen's header shows (DESIGN.md SS8). The device name is off by default:
// it is useful on the web page, not on the device that carries it, and the header is narrow.
struct HeaderConfig {
  bool name = false;
  bool clock = true;
  bool weather = true;
  bool wifi = true;
  bool updated = true;
};

// DESIGN.md SS6 "quiet": backlight schedule; a touch wakes the screen for wake_seconds.
struct QuietConfig {
  bool enabled = false;
  std::string start = "23:00";  // local "HH:MM"
  std::string end = "06:00";    // may be earlier than start (crosses midnight)
  uint8_t brightness = 0;       // 0..50 percent, 0 = off
  uint16_t wake_seconds = 30;
};

// DESIGN.md SS6 "night": clock page when nothing is due within after_min.
struct NightConfig {
  bool enabled = true;
  uint16_t after_min = 60;
};

// DESIGN.md SS6 "due": time-to-leave alert when an arrival first comes within `minutes`.
struct DueConfig {
  bool enabled = true;
  uint8_t minutes = 3;
  bool led = true;
  bool screen = true;
  bool chime = false;
};

// DESIGN.md SS6 "profiles": which stops the main page shows during a time window.
struct ProfileConfig {
  std::string name;
  uint8_t days = 0;  // bitmask, bit 0 = Sunday .. bit 6 = Saturday
  std::string start = "05:30";
  std::string end = "10:00";
  std::vector<std::string> stops;  // StopConfig::key, in display order
};
constexpr size_t kMaxProfiles = 4;

// DESIGN.md SS6 "bike" / SS4.9: Indego stations to show.
struct BikeStation {
  int id = 0;
  std::string name;
};
struct BikeConfig {
  bool enabled = false;
  std::vector<BikeStation> stations;  // at most kMaxBikeStations
};
constexpr size_t kMaxBikeStations = 3;

// DESIGN.md SS4.8: Open-Meteo forecasts for the configured stops' locations.
struct WeatherConfig {
  bool enabled = true;
  bool per_stop = true;     // one-line note on a stop panel when its next arrival's hour differs
  bool fahrenheit = true;   // JSON "units": "f" | "c"
};

struct DeviceConfig {
  std::string name = "transit-display";
  std::string tz = "EST5EDT,M3.2.0,M11.1.0";
  uint16_t poll_seconds = 30;
  uint8_t brightness = 80;
  uint16_t rotation = 0;  // 0, 90, 180, 270 degrees; 0 = panel-native portrait (DESIGN.md SS6)
  std::string theme = "light";  // "light" | "dark" (ui_common.h setTheme)
  bool invert_colors = DISPLAY_INVERT_DEFAULT != 0;  // panel colour inversion (ui.h applyInvert)
  std::string ticker_show = "both";  // both | alerts | detours | off (DESIGN.md SS6)
  uint8_t ticker_lines = 3;     // alert ticker height in text lines, 1..8 (1 = horizontal marquee)
  uint16_t ticker_speed = 30;   // alert ticker scroll speed in pixels per second, 5..200
  bool tls_verify = true;
  bool use_https = false;  // see http_fetch.h: TLS is a 40 KB luxury this board cannot afford by default
  bool logging = true;
  HeaderConfig header;
  bool large_text = false;     // two rows per stop, 48 px minutes (ui_common.cpp fontBig)
  // SEPTA seat availability next to the destination: off | words | icons | both, and which icon
  // scheme: seats (chairs then people) | crowd (people only). Older configs carried a boolean
  // show_crowding; fromJson still reads it (DESIGN.md SS6).
  std::string crowding = "words";
  std::string crowding_icons = "seats";
  QuietConfig quiet;
  NightConfig night;
};

struct Config {
  int version = 1;
  DeviceConfig device;
  std::vector<transit::StopConfig> stops;
  bool alerts = true;
  WeatherConfig weather;
  DueConfig due;
  std::vector<ProfileConfig> profiles;
  BikeConfig bike;
};

// { "error": "...", "path": "stops[1].stop_id" } - DESIGN.md SS6's shape for
// a 400 response from PUT /api/config.
struct ConfigError {
  std::string message;
  std::string path;
};

// The config this project ships with on a blank device: DESIGN.md SS6's
// example device block, plus the owner's two Route 17 stops (SS1: 19th &
// Mifflin southbound / 20th & Mifflin northbound). Does NOT include SS6's
// third example stop (a Regional Rail row) - that entry in the design doc
// is illustrating the schema, not part of the actual default.
Config defaultConfig();

// Checks `cfg` against DESIGN.md SS6's rules (stop count, required fields
// per mode, unique keys, sane ranges). Returns true if valid; otherwise
// fills `err` and returns false. Does not touch the filesystem.
bool validateConfig(const Config &cfg, ConfigError &err);

// Reads and parses kConfigPath from an already-mounted LittleFS. Returns
// false (leaving `cfg` unchanged) if the file is missing, unreadable,
// malformed JSON, or fails validateConfig(). Callers should fall back to
// defaultConfig() + saveConfig() in that case (see main.cpp).
bool loadConfig(Config &cfg);

// Serializes `cfg` and writes it to kConfigPath, replacing any existing
// file. Does not validate - call validateConfig() first if the source is
// untrusted (e.g. a PUT /api/config body). Returns false on a filesystem
// error.
bool saveConfig(const Config &cfg);

// JSON <-> Config, split out from load/saveConfig so web_server.cpp can
// reuse the same (de)serialization for GET/PUT /api/config without a round
// trip through the filesystem.
void configToJson(const Config &cfg, JsonDocument &doc);
bool jsonToConfig(const JsonVariant &doc, Config &cfg, ConfigError &err);

// Thread-safe holder for the config currently in effect. main.cpp calls
// setActiveConfig() once in setup(), right after loadConfig()/
// defaultConfig(); after that, web_server.cpp (running on the async web
// server's task) and any other task can call getActiveConfig() for a safe
// copy without racing main's or each other's access. A PUT /api/config
// that validates and saves successfully calls setActiveConfig() again with
// the new value.
Config getActiveConfig();
void setActiveConfig(const Config &cfg);

}  // namespace transit_app
