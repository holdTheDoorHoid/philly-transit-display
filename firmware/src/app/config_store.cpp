#include "config_store.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <set>

using transit::Mode;
using transit::StopConfig;

namespace transit_app {

namespace {

const char *modeToString(Mode m) {
  switch (m) {
    case Mode::Bus:
      return "bus";
    case Mode::Trolley:
      return "trolley";
    case Mode::Subway:
      return "subway";
    case Mode::Rail:
      return "rail";
  }
  return "bus";
}

bool stringToMode(const std::string &s, Mode &out) {
  if (s == "bus") {
    out = Mode::Bus;
  } else if (s == "trolley") {
    out = Mode::Trolley;
  } else if (s == "subway") {
    out = Mode::Subway;
  } else if (s == "rail") {
    out = Mode::Rail;
  } else {
    return false;
  }
  return true;
}

std::string field(const char *path, size_t index, const char *suffix) {
  std::string p(path);
  p += "[" + std::to_string(index) + "]." + suffix;
  return p;
}

// "HH:MM", 00:00..23:59.
bool validClock(const std::string &s) {
  if (s.size() != 5 || s[2] != ':') return false;
  for (size_t i : {0u, 1u, 3u, 4u}) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  int hh = (s[0] - '0') * 10 + (s[1] - '0');
  int mm = (s[3] - '0') * 10 + (s[4] - '0');
  return hh <= 23 && mm <= 59;
}

bool validTitleStyle(const std::string &s) {
  return s == "label_dest" || s == "label" || s == "route_dest_stop" || s == "custom";
}

const StopConfig *findStopByKey(const std::vector<StopConfig> &stops, const std::string &key) {
  for (const StopConfig &s : stops) {
    if (s.key == key) return &s;
  }
  return nullptr;
}

}  // namespace

Config defaultConfig() {
  Config cfg;
  cfg.version = 1;
  cfg.device = DeviceConfig{};  // struct defaults already match DESIGN.md SS6
  cfg.alerts = true;

  StopConfig southbound;
  southbound.key = "17-21332";
  southbound.mode = Mode::Bus;
  southbound.route = "17";
  southbound.stop_id = "21332";
  southbound.direction = "1";
  southbound.headsign = "20th-Johnston";
  southbound.label = "17 Southbound";
  southbound.stop_name = "19th St & Mifflin St";
  southbound.show = 3;
  southbound.lat = 39.927947;  // SEPTA Stops API, route 17
  southbound.lng = -75.177147;

  StopConfig northbound;
  northbound.key = "17-21297";
  northbound.mode = Mode::Bus;
  northbound.route = "17";
  northbound.stop_id = "21297";
  northbound.direction = "0";
  northbound.headsign = "2nd-Market";
  northbound.label = "17 Northbound";
  northbound.stop_name = "20th St & Mifflin St";
  northbound.show = 3;
  northbound.lat = 39.927942;
  northbound.lng = -75.178646;

  cfg.stops = {southbound, northbound};
  return cfg;
}

bool validateConfig(const Config &cfg, ConfigError &err) {
  if (cfg.stops.size() > kMaxStops) {
    err = {"at most " + std::to_string(kMaxStops) + " stops are allowed", "stops"};
    return false;
  }
  if (cfg.device.poll_seconds < 5 || cfg.device.poll_seconds > 600) {
    err = {"poll_seconds must be between 5 and 600", "device.poll_seconds"};
    return false;
  }
  if (cfg.device.brightness > 100) {
    err = {"brightness must be between 0 and 100", "device.brightness"};
    return false;
  }
  if (cfg.device.rotation != 0 && cfg.device.rotation != 90 && cfg.device.rotation != 180 && cfg.device.rotation != 270) {
    err = {"rotation must be 0, 90, 180, or 270", "device.rotation"};
    return false;
  }
  if (cfg.device.name.empty()) {
    err = {"device.name must not be empty (used as the mDNS hostname)", "device.name"};
    return false;
  }
  if (cfg.device.theme != "light" && cfg.device.theme != "dark") {
    err = {"theme must be \"light\" or \"dark\"", "device.theme"};
    return false;
  }
  if (cfg.device.ticker_show != "both" && cfg.device.ticker_show != "alerts" && cfg.device.ticker_show != "detours" &&
      cfg.device.ticker_show != "off") {
    err = {"ticker_show must be both, alerts, detours, or off", "device.ticker_show"};
    return false;
  }
  if (cfg.device.crowding != "off" && cfg.device.crowding != "words" && cfg.device.crowding != "icons" &&
      cfg.device.crowding != "both") {
    err = {"crowding must be off, words, icons, or both", "device.crowding"};
    return false;
  }
  if (cfg.device.crowding_icons != "seats" && cfg.device.crowding_icons != "crowd") {
    err = {"crowding_icons must be seats or crowd", "device.crowding_icons"};
    return false;
  }
  if (cfg.device.ticker_lines < 1 || cfg.device.ticker_lines > 8) {
    err = {"ticker_lines must be between 1 and 8", "device.ticker_lines"};
    return false;
  }
  if (cfg.device.ticker_speed < 5 || cfg.device.ticker_speed > 200) {
    err = {"ticker_speed must be between 5 and 200 pixels per second", "device.ticker_speed"};
    return false;
  }
  for (size_t i = 0; i < cfg.stops.size(); ++i) {
    const StopConfig &s = cfg.stops[i];
    if (s.lat < -90 || s.lat > 90 || s.lng < -180 || s.lng > 180) {
      err = {"lat/lng out of range", field("stops", i, "lat")};
      return false;
    }
    if (!validTitleStyle(s.title_style)) {
      err = {"title_style must be label_dest, label, route_dest_stop, or custom", field("stops", i, "title_style")};
      return false;
    }
    if (s.title_text.size() > 40) {
      err = {"title_text must be at most 40 characters", field("stops", i, "title_text")};
      return false;
    }
    if (!s.alt_of.empty()) {
      if (s.alt_of == s.key || findStopByKey(cfg.stops, s.alt_of) == nullptr) {
        err = {"alt_of must be the key of another configured stop", field("stops", i, "alt_of")};
        return false;
      }
      if (s.alt_after_min < 5 || s.alt_after_min > 60) {
        err = {"alt_after_min must be between 5 and 60", field("stops", i, "alt_after_min")};
        return false;
      }
    }
  }
  if (!validClock(cfg.device.quiet.start) || !validClock(cfg.device.quiet.end)) {
    err = {"quiet.start and quiet.end must be HH:MM", "device.quiet.start"};
    return false;
  }
  if (cfg.device.quiet.brightness > 50) {
    err = {"quiet.brightness must be between 0 and 50", "device.quiet.brightness"};
    return false;
  }
  if (cfg.device.quiet.wake_seconds < 5 || cfg.device.quiet.wake_seconds > 300) {
    err = {"quiet.wake_seconds must be between 5 and 300", "device.quiet.wake_seconds"};
    return false;
  }
  if (cfg.device.night.after_min < 15 || cfg.device.night.after_min > 240) {
    err = {"night.after_min must be between 15 and 240", "device.night.after_min"};
    return false;
  }
  if (cfg.due.minutes < 1 || cfg.due.minutes > 15) {
    err = {"due.minutes must be between 1 and 15", "due.minutes"};
    return false;
  }
  if (cfg.profiles.size() > kMaxProfiles) {
    err = {"at most " + std::to_string(kMaxProfiles) + " profiles are allowed", "profiles"};
    return false;
  }
  for (size_t i = 0; i < cfg.profiles.size(); ++i) {
    const ProfileConfig &p = cfg.profiles[i];
    if (p.name.empty() || p.name.size() > 24) {
      err = {"profile name must be 1-24 characters", field("profiles", i, "name")};
      return false;
    }
    if (!validClock(p.start) || !validClock(p.end)) {
      err = {"profile start and end must be HH:MM", field("profiles", i, "start")};
      return false;
    }
    if (p.stops.empty()) {
      err = {"a profile needs at least one stop", field("profiles", i, "stops")};
      return false;
    }
    for (const std::string &k : p.stops) {
      if (findStopByKey(cfg.stops, k) == nullptr) {
        err = {"profile stop '" + k + "' is not a configured stop", field("profiles", i, "stops")};
        return false;
      }
    }
  }
  if (cfg.bike.style != "icons" && cfg.bike.style != "words") {
    err = {"style must be icons or words", "bike.style"};
    return false;
  }
  if (cfg.bike.stations.size() > kMaxBikeStations) {
    err = {"at most " + std::to_string(kMaxBikeStations) + " bike stations are allowed", "bike.stations"};
    return false;
  }
  for (size_t i = 0; i < cfg.bike.stations.size(); ++i) {
    if (cfg.bike.stations[i].id < 1) {
      err = {"station id must be a positive integer", field("bike.stations", i, "id")};
      return false;
    }
  }

  std::set<std::string> seen_keys;
  for (size_t i = 0; i < cfg.stops.size(); ++i) {
    const StopConfig &s = cfg.stops[i];
    if (s.key.empty()) {
      err = {"key must not be empty", field("stops", i, "key")};
      return false;
    }
    if (!seen_keys.insert(s.key).second) {
      err = {"key '" + s.key + "' is not unique", field("stops", i, "key")};
      return false;
    }
    if (s.show < 1 || s.show > 4) {
      err = {"show must be between 1 and 4", field("stops", i, "show")};
      return false;
    }
    if (s.mode == Mode::Rail) {
      if (s.station.empty()) {
        err = {"station is required for mode \"rail\"", field("stops", i, "station")};
        return false;
      }
    } else {
      if (s.route.empty()) {
        err = {"route is required for this mode", field("stops", i, "route")};
        return false;
      }
      if (s.stop_id.empty()) {
        err = {"stop_id is required for this mode", field("stops", i, "stop_id")};
        return false;
      }
    }
  }
  return true;
}

void configToJson(const Config &cfg, JsonDocument &doc) {
  doc.clear();
  doc["version"] = cfg.version;

  JsonObject device = doc["device"].to<JsonObject>();
  device["name"] = cfg.device.name;
  device["tz"] = cfg.device.tz;
  device["poll_seconds"] = cfg.device.poll_seconds;
  device["brightness"] = cfg.device.brightness;
  device["rotation"] = cfg.device.rotation;
  device["theme"] = cfg.device.theme;
  device["invert_colors"] = cfg.device.invert_colors;
  device["ticker_show"] = cfg.device.ticker_show;
  device["ticker_lines"] = cfg.device.ticker_lines;
  device["ticker_speed"] = cfg.device.ticker_speed;
  device["logging"] = cfg.device.logging;
  JsonObject header = device["header"].to<JsonObject>();
  header["name"] = cfg.device.header.name;
  header["clock"] = cfg.device.header.clock;
  header["weather"] = cfg.device.header.weather;
  header["wifi"] = cfg.device.header.wifi;
  header["updated"] = cfg.device.header.updated;
  device["large_text"] = cfg.device.large_text;
  device["crowding"] = cfg.device.crowding;
  device["crowding_icons"] = cfg.device.crowding_icons;
  JsonObject quiet = device["quiet"].to<JsonObject>();
  quiet["enabled"] = cfg.device.quiet.enabled;
  quiet["start"] = cfg.device.quiet.start;
  quiet["end"] = cfg.device.quiet.end;
  quiet["brightness"] = cfg.device.quiet.brightness;
  quiet["wake_seconds"] = cfg.device.quiet.wake_seconds;
  JsonObject night = device["night"].to<JsonObject>();
  night["enabled"] = cfg.device.night.enabled;
  night["after_min"] = cfg.device.night.after_min;

  JsonArray stops = doc["stops"].to<JsonArray>();
  for (const StopConfig &s : cfg.stops) {
    JsonObject o = stops.add<JsonObject>();
    o["key"] = s.key;
    o["mode"] = modeToString(s.mode);
    if (s.mode == Mode::Rail) {
      o["station"] = s.station;
      o["direction"] = s.direction;
      o["line"] = s.route;  // see StopConfig::route's doc comment in model.h
    } else {
      o["route"] = s.route;
      o["stop_id"] = s.stop_id;
      o["direction"] = s.direction;
    }
    o["headsign"] = s.headsign;
    o["label"] = s.label;
    o["stop_name"] = s.stop_name;
    o["show"] = s.show;
    if (s.lat != 0 || s.lng != 0) {
      o["lat"] = s.lat;
      o["lng"] = s.lng;
    }
    o["title_style"] = s.title_style;
    o["title_text"] = s.title_text;
    o["alt_of"] = s.alt_of;
    o["alt_after_min"] = s.alt_after_min;
  }

  doc["alerts"] = cfg.alerts;
  JsonObject weather = doc["weather"].to<JsonObject>();
  weather["enabled"] = cfg.weather.enabled;
  weather["per_stop"] = cfg.weather.per_stop;
  weather["units"] = cfg.weather.fahrenheit ? "f" : "c";
  JsonObject due = doc["due"].to<JsonObject>();
  due["enabled"] = cfg.due.enabled;
  due["minutes"] = cfg.due.minutes;
  due["led"] = cfg.due.led;
  due["screen"] = cfg.due.screen;
  due["chime"] = cfg.due.chime;
  JsonArray profiles = doc["profiles"].to<JsonArray>();
  for (const ProfileConfig &p : cfg.profiles) {
    JsonObject po = profiles.add<JsonObject>();
    po["name"] = p.name;
    JsonArray days = po["days"].to<JsonArray>();
    for (int d = 0; d < 7; ++d) {
      if (p.days & (1u << d)) days.add(d);
    }
    po["start"] = p.start;
    po["end"] = p.end;
    JsonArray ps = po["stops"].to<JsonArray>();
    for (const std::string &k : p.stops) ps.add(k);
  }
  JsonObject bike = doc["bike"].to<JsonObject>();
  bike["enabled"] = cfg.bike.enabled;
  bike["style"] = cfg.bike.style;
  JsonArray stations = bike["stations"].to<JsonArray>();
  for (const BikeStation &b : cfg.bike.stations) {
    JsonObject bo = stations.add<JsonObject>();
    bo["id"] = b.id;
    bo["name"] = b.name;
  }
}

bool jsonToConfig(const JsonVariant &doc, Config &cfg, ConfigError &err) {
  Config result;
  result.version = doc["version"] | 1;

  JsonVariantConst device = doc["device"];
  result.device.name = std::string(device["name"] | "transit-display");
  result.device.tz = std::string(device["tz"] | "EST5EDT,M3.2.0,M11.1.0");
  result.device.poll_seconds = device["poll_seconds"] | 30;
  result.device.brightness = device["brightness"] | 80;
  result.device.rotation = device["rotation"] | 0;
  result.device.theme = std::string(device["theme"] | "light");
  result.device.invert_colors = device["invert_colors"] | (DISPLAY_INVERT_DEFAULT != 0);
  result.device.ticker_show = std::string(device["ticker_show"] | "both");
  result.device.ticker_lines = device["ticker_lines"] | 3;
  result.device.ticker_speed = device["ticker_speed"] | 30;
  // use_https / tls_verify (v0.1.0-0.1.1) are accepted and ignored: the TLS mode is gone (http_fetch.h).
  result.device.logging = device["logging"] | true;
  JsonVariantConst header = device["header"];
  result.device.header.name = header["name"] | false;
  result.device.header.clock = header["clock"] | true;
  result.device.header.weather = header["weather"] | true;
  result.device.header.wifi = header["wifi"] | true;
  result.device.header.updated = header["updated"] | true;
  result.device.large_text = device["large_text"] | false;
  // v0.1.0 configs stored a boolean show_crowding; honour it when the newer string is absent.
  if (device["crowding"].is<const char *>()) {
    result.device.crowding = std::string(device["crowding"].as<const char *>());
  } else {
    result.device.crowding = (device["show_crowding"] | true) ? "words" : "off";
  }
  result.device.crowding_icons = std::string(device["crowding_icons"] | "seats");
  JsonVariantConst quiet = device["quiet"];
  result.device.quiet.enabled = quiet["enabled"] | false;
  result.device.quiet.start = std::string(quiet["start"] | "23:00");
  result.device.quiet.end = std::string(quiet["end"] | "06:00");
  result.device.quiet.brightness = quiet["brightness"] | 0;
  result.device.quiet.wake_seconds = quiet["wake_seconds"] | 30;
  JsonVariantConst night = device["night"];
  result.device.night.enabled = night["enabled"] | true;
  result.device.night.after_min = night["after_min"] | 60;

  JsonVariantConst stops = doc["stops"];
  if (!stops.isNull()) {
    if (!stops.is<JsonArrayConst>()) {
      err = {"stops must be an array", "stops"};
      return false;
    }
    size_t i = 0;
    for (JsonVariantConst v : stops.as<JsonArrayConst>()) {
      StopConfig s;
      s.key = std::string(v["key"] | "");
      std::string mode_str = std::string(v["mode"] | "bus");
      if (!stringToMode(mode_str, s.mode)) {
        err = {"unknown mode \"" + mode_str + "\" (expected bus/trolley/subway/rail)", field("stops", i, "mode")};
        return false;
      }
      s.station = std::string(v["station"] | "");
      s.direction = std::string(v["direction"] | "");
      s.headsign = std::string(v["headsign"] | "");
      s.label = std::string(v["label"] | "");
      s.stop_name = std::string(v["stop_name"] | "");
      s.show = v["show"] | 3;
      s.lat = v["lat"] | 0.0;
      s.lng = v["lng"] | 0.0;
      s.title_style = std::string(v["title_style"] | "label_dest");
      s.title_text = std::string(v["title_text"] | "");
      s.alt_of = std::string(v["alt_of"] | "");
      s.alt_after_min = v["alt_after_min"] | 15;
      if (s.mode == Mode::Rail) {
        // DESIGN.md SS6's rail example uses "line", not "route"; model.h's
        // StopConfig folds both into `route` (see its doc comment).
        s.route = std::string(v["line"] | "");
        s.stop_id = "";
      } else {
        s.route = std::string(v["route"] | "");
        s.stop_id = std::string(v["stop_id"] | "");
      }
      result.stops.push_back(s);
      ++i;
    }
  }

  result.alerts = doc["alerts"] | true;
  JsonVariantConst weather = doc["weather"];
  result.weather.enabled = weather["enabled"] | true;
  result.weather.per_stop = weather["per_stop"] | true;
  std::string units = std::string(weather["units"] | "f");
  if (units != "f" && units != "c") {
    err = {"weather.units must be \"f\" or \"c\"", "weather.units"};
    return false;
  }
  result.weather.fahrenheit = (units == "f");
  JsonVariantConst due = doc["due"];
  result.due.enabled = due["enabled"] | true;
  result.due.minutes = due["minutes"] | 3;
  result.due.led = due["led"] | true;
  result.due.screen = due["screen"] | true;
  result.due.chime = due["chime"] | false;
  JsonVariantConst profiles = doc["profiles"];
  if (profiles.is<JsonArrayConst>()) {
    for (JsonVariantConst pv : profiles.as<JsonArrayConst>()) {
      ProfileConfig p;
      p.name = std::string(pv["name"] | "");
      JsonVariantConst days = pv["days"];
      if (days.is<JsonArrayConst>()) {
        for (JsonVariantConst d : days.as<JsonArrayConst>()) {
          int day = d | -1;
          if (day >= 0 && day <= 6) p.days |= (uint8_t)(1u << day);
        }
      }
      p.start = std::string(pv["start"] | "05:30");
      p.end = std::string(pv["end"] | "10:00");
      JsonVariantConst ps = pv["stops"];
      if (ps.is<JsonArrayConst>()) {
        for (JsonVariantConst k : ps.as<JsonArrayConst>()) p.stops.push_back(std::string(k | ""));
      }
      result.profiles.push_back(p);
    }
  }
  JsonVariantConst bike = doc["bike"];
  result.bike.enabled = bike["enabled"] | false;
  result.bike.style = std::string(bike["style"] | "icons");
  JsonVariantConst stations = bike["stations"];
  if (stations.is<JsonArrayConst>()) {
    for (JsonVariantConst sv : stations.as<JsonArrayConst>()) {
      BikeStation b;
      b.id = sv["id"] | 0;
      b.name = std::string(sv["name"] | "");
      result.bike.stations.push_back(b);
    }
  }

  // Configs saved before stops carried coordinates: the two default stops get theirs back so
  // weather works without re-adding them (the web UI offers a lookup for any other stop).
  {
    Config defaults = defaultConfig();
    for (StopConfig &s : result.stops) {
      if (s.lat != 0 || s.lng != 0) continue;
      for (const StopConfig &d : defaults.stops) {
        if (d.key == s.key && d.stop_id == s.stop_id) {
          s.lat = d.lat;
          s.lng = d.lng;
        }
      }
    }
  }

  if (!validateConfig(result, err)) {
    return false;
  }
  cfg = result;
  return true;
}

bool loadConfig(Config &cfg) {
  if (!LittleFS.exists(kConfigPath)) {
    log_w("config_store: %s does not exist", kConfigPath);
    return false;
  }
  File f = LittleFS.open(kConfigPath, "r");
  if (!f) {
    log_e("config_store: failed to open %s for reading", kConfigPath);
    return false;
  }

  JsonDocument doc;
  DeserializationError parse_err = deserializeJson(doc, f);
  f.close();
  if (parse_err) {
    log_e("config_store: %s is not valid JSON: %s", kConfigPath, parse_err.c_str());
    return false;
  }

  ConfigError verr;
  Config parsed;
  if (!jsonToConfig(doc.as<JsonVariant>(), parsed, verr)) {
    log_e("config_store: %s failed validation at %s: %s", kConfigPath, verr.path.c_str(), verr.message.c_str());
    return false;
  }
  cfg = parsed;
  return true;
}

bool saveConfig(const Config &cfg) {
  JsonDocument doc;
  configToJson(cfg, doc);

  File f = LittleFS.open(kConfigPath, "w");
  if (!f) {
    log_e("config_store: failed to open %s for writing", kConfigPath);
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.close();
  if (written == 0) {
    log_e("config_store: serializeJson wrote 0 bytes to %s", kConfigPath);
    return false;
  }
  return true;
}

namespace {
SemaphoreHandle_t g_active_mutex = nullptr;
Config g_active_config;

SemaphoreHandle_t activeMutex() {
  if (g_active_mutex == nullptr) {
    g_active_mutex = xSemaphoreCreateMutex();
  }
  return g_active_mutex;
}
}  // namespace

Config getActiveConfig() {
  Config copy;
  SemaphoreHandle_t mutex = activeMutex();
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    copy = g_active_config;
    xSemaphoreGive(mutex);
  }
  return copy;
}

void setActiveConfig(const Config &cfg) {
  SemaphoreHandle_t mutex = activeMutex();
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    g_active_config = cfg;
    xSemaphoreGive(mutex);
  }
}

}  // namespace transit_app
