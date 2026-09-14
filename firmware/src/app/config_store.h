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
// only with the controller's inversion command on (IPS variants do). Runtime-overridable via
// device.invert_colors; see docs/hardware.md "Colour inversion".
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
  uint8_t ticker_lines = 3;     // alert ticker height in text lines, 1..8 (1 = horizontal marquee)
  uint16_t ticker_speed = 30;   // alert ticker scroll speed in pixels per second, 5..200
  bool tls_verify = true;
  bool use_https = false;  // see http_fetch.h: TLS is a 40 KB luxury this board cannot afford by default
  bool logging = true;
  HeaderConfig header;
};

struct Config {
  int version = 1;
  DeviceConfig device;
  std::vector<transit::StopConfig> stops;
  bool alerts = true;
  WeatherConfig weather;
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
