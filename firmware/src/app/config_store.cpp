#include "config_store.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cctype>
#include <cmath>
#include <set>

#include "transit_core/septa.h"

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

// ---------------------------------------------------------------------------
// Bounded, type-checked ingestion (DESIGN.md SS6; review F07)
// ---------------------------------------------------------------------------
//
// WHY these helpers exist: the old reader used ArduinoJson's `doc["x"] | default` idiom straight
// into the destination field, which narrows silently. `brightness: 256` became 0 in a uint8_t and
// `poll_seconds: 65566` became 30 in a uint16_t - both then passed validateConfig() and were
// saved, so a PUT that should have been a 400 quietly wrote a config the owner did not ask for.
// Every number is now read as int64/double, range-checked against the range the destination type
// and DESIGN.md SS6 allow, and only then narrowed; every string is length-checked and rejected for
// control characters while it is still a JsonVariant, before it is copied anywhere.
//
// Booleans keep the `|` idiom on purpose: a wrong-typed boolean falls back to the documented
// default, which cannot wrap, truncate or grow a buffer.

constexpr size_t kMaxNameLen = 32;        // also the mDNS label limit in practice
constexpr size_t kMaxTzLen = 64;          // POSIX TZ strings are far shorter than this
constexpr size_t kMaxStopFieldLen = 64;   // key/route/stop_id/direction/headsign/label/stop_name/station/title_text/alt_of
constexpr size_t kMaxProfileNameLen = 32;
constexpr size_t kMaxBikeNameLen = 48;
constexpr size_t kMaxEnumLen = 32;        // theme, ticker_show, crowding, units, style ... validated by value later
constexpr size_t kMaxClockLen = 8;        // "HH:MM"
constexpr size_t kMaxDaysEntries = 7;

bool hasControlChars(const std::string &s) {
  for (char c : s) {
    unsigned char u = (unsigned char)c;
    if (u < 0x20 || u == 0x7F) return true;
  }
  return false;
}

// One JSON value -> one bounded std::string. Shared by readStr() (object fields) and the bare
// string arrays (a profile's stop keys).
bool checkStr(JsonVariantConst v, size_t max_len, const std::string &path, std::string &out, ConfigError &err) {
  if (!v.is<const char *>()) {
    err = {path + " must be a string", path};
    return false;
  }
  const char *s = v.as<const char *>();
  out.assign(s != nullptr ? s : "");
  if (out.size() > max_len) {
    err = {path + " must be at most " + std::to_string(max_len) + " characters", path};
    return false;
  }
  if (hasControlChars(out)) {
    // A newline or NUL in a label would break the CSV log, the JSON echo and the LVGL label all
    // at once; there is no legitimate config value that contains one.
    err = {path + " must not contain control characters", path};
    return false;
  }
  return true;
}

bool readStr(JsonVariantConst parent, const char *key, size_t max_len, const char *def, const std::string &path, std::string &out,
             ConfigError &err) {
  JsonVariantConst v = parent[key];
  if (v.isNull()) {
    out = def;
    return true;
  }
  return checkStr(v, max_len, path, out, err);
}

bool readInt(JsonVariantConst parent, const char *key, int64_t lo, int64_t hi, int64_t def, const std::string &path, int64_t &out,
             ConfigError &err) {
  JsonVariantConst v = parent[key];
  if (v.isNull()) {
    out = def;
    return true;
  }
  if (!v.is<int64_t>()) {
    err = {path + " must be a whole number", path};
    return false;
  }
  out = v.as<int64_t>();
  if (out < lo || out > hi) {
    err = {path + " must be between " + std::to_string(lo) + " and " + std::to_string(hi), path};
    return false;
  }
  return true;
}

bool readNum(JsonVariantConst parent, const char *key, double lo, double hi, double def, const std::string &path, double &out,
             ConfigError &err) {
  JsonVariantConst v = parent[key];
  if (v.isNull()) {
    out = def;
    return true;
  }
  if (!v.is<double>()) {
    err = {path + " must be a number", path};
    return false;
  }
  out = v.as<double>();
  if (!std::isfinite(out)) {
    // "1e400" parses to infinity rather than failing; an infinite latitude then poisons every
    // distance/weather calculation downstream (DESIGN.md SS4.8).
    err = {path + " must be a finite number", path};
    return false;
  }
  if (out < lo || out > hi) {
    err = {path + " must be between " + std::to_string((int)lo) + " and " + std::to_string((int)hi), path};
    return false;
  }
  return true;
}

// The device name becomes "<name>.local", the mDNS service instance and the DHCP hostname, where
// anything outside [a-z0-9-] is either refused by the resolver or silently mangled. DNS is
// case-insensitive, so an upper-case name is lowered here (the "slugify" step) rather than
// refused; a character with no slug is refused, because guessing what the owner meant by
// "Mike's Display" would produce a name they cannot predict or type.
bool slugifyDeviceName(std::string &name, ConfigError &err) {
  for (char &c : name) c = (char)tolower((unsigned char)c);
  if (name.empty()) return true;  // validateConfig() owns the "must not be empty" message
  for (char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') continue;
    err = {"device.name may only contain letters, digits and '-' (it is used as the mDNS hostname)", "device.name"};
    return false;
  }
  if (name.front() == '-' || name.back() == '-') {
    err = {"device.name must not start or end with '-'", "device.name"};
    return false;
  }
  return true;
}

bool equalsIgnoreCase(const std::string &a, const char *b) {
  size_t i = 0;
  for (; i < a.size(); ++i) {
    if (b[i] == '\0') return false;
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return b[i] == '\0';
}

// A Regional Rail stop's line may arrive as the code ("PAO") or as the display name that the
// SEPTA Arrivals/TrainView responses and the web UI's own picker use ("Paoli/Thorndale") - review
// F30. Only the code matches anything downstream (transit_core/septa.h's mergeRail() and the
// alert suffix), so a display name used to be stored verbatim and then silently matched no
// trains at all. Normalise to the code here, once, at the only place configs enter the system.
// An empty line stays empty: DESIGN.md SS6 documents that as "all lines at this station".
bool normalizeRailLine(std::string &line, const std::string &path, ConfigError &err) {
  if (line.empty()) return true;
  for (size_t i = 0; i < transit::kRailLineCount; ++i) {
    if (equalsIgnoreCase(line, transit::kRailLines[i].code)) {
      line = transit::kRailLines[i].code;  // canonical upper case
      return true;
    }
  }
  for (size_t i = 0; i < transit::kRailLineCount; ++i) {
    if (equalsIgnoreCase(line, transit::kRailLines[i].display_name)) {
      line = transit::kRailLines[i].code;
      return true;
    }
  }
  err = {"unknown Regional Rail line; use a line code such as PAO", path};
  return false;
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
  int64_t n = 0;
  double d = 0;

  if (!readInt(doc, "version", 0, 1000, 1, "version", n, err)) return false;
  result.version = (int)n;

  JsonVariantConst device = doc["device"];
  if (!readStr(device, "name", kMaxNameLen, "transit-display", "device.name", result.device.name, err)) return false;
  if (!slugifyDeviceName(result.device.name, err)) return false;
  if (!readStr(device, "tz", kMaxTzLen, "EST5EDT,M3.2.0,M11.1.0", "device.tz", result.device.tz, err)) return false;
  if (!readInt(device, "poll_seconds", 5, 600, 30, "device.poll_seconds", n, err)) return false;
  result.device.poll_seconds = (uint16_t)n;
  if (!readInt(device, "brightness", 0, 100, 80, "device.brightness", n, err)) return false;
  result.device.brightness = (uint8_t)n;
  if (!readInt(device, "rotation", 0, 270, 0, "device.rotation", n, err)) return false;
  result.device.rotation = (uint16_t)n;  // validateConfig() rejects anything but 0/90/180/270
  if (!readStr(device, "theme", kMaxEnumLen, "light", "device.theme", result.device.theme, err)) return false;
  result.device.invert_colors = device["invert_colors"] | (DISPLAY_INVERT_DEFAULT != 0);
  if (!readStr(device, "ticker_show", kMaxEnumLen, "both", "device.ticker_show", result.device.ticker_show, err)) return false;
  if (!readInt(device, "ticker_lines", 1, 8, 3, "device.ticker_lines", n, err)) return false;
  result.device.ticker_lines = (uint8_t)n;
  if (!readInt(device, "ticker_speed", 5, 200, 30, "device.ticker_speed", n, err)) return false;
  result.device.ticker_speed = (uint16_t)n;
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
  if (device["crowding"].isNull()) {
    result.device.crowding = (device["show_crowding"] | true) ? "words" : "off";
  } else if (!readStr(device, "crowding", kMaxEnumLen, "words", "device.crowding", result.device.crowding, err)) {
    return false;
  }
  if (!readStr(device, "crowding_icons", kMaxEnumLen, "seats", "device.crowding_icons", result.device.crowding_icons, err)) return false;
  JsonVariantConst quiet = device["quiet"];
  result.device.quiet.enabled = quiet["enabled"] | false;
  if (!readStr(quiet, "start", kMaxClockLen, "23:00", "device.quiet.start", result.device.quiet.start, err)) return false;
  if (!readStr(quiet, "end", kMaxClockLen, "06:00", "device.quiet.end", result.device.quiet.end, err)) return false;
  if (!readInt(quiet, "brightness", 0, 50, 0, "device.quiet.brightness", n, err)) return false;
  result.device.quiet.brightness = (uint8_t)n;
  if (!readInt(quiet, "wake_seconds", 5, 300, 30, "device.quiet.wake_seconds", n, err)) return false;
  result.device.quiet.wake_seconds = (uint16_t)n;
  JsonVariantConst night = device["night"];
  result.device.night.enabled = night["enabled"] | true;
  if (!readInt(night, "after_min", 15, 240, 60, "device.night.after_min", n, err)) return false;
  result.device.night.after_min = (uint16_t)n;

  JsonVariantConst stops = doc["stops"];
  if (!stops.isNull()) {
    if (!stops.is<JsonArrayConst>()) {
      err = {"stops must be an array", "stops"};
      return false;
    }
    JsonArrayConst arr = stops.as<JsonArrayConst>();
    // Capped while ingesting, not after: eight StopConfigs is ~1 KB of std::string on a heap with
    // ~75 KB free, and a PUT with a thousand of them must not be built before it is refused.
    if (arr.size() > kMaxStops) {
      err = {"at most " + std::to_string(kMaxStops) + " stops are allowed", "stops"};
      return false;
    }
    size_t i = 0;
    for (JsonVariantConst v : arr) {
      StopConfig s;
      if (!readStr(v, "key", kMaxStopFieldLen, "", field("stops", i, "key"), s.key, err)) return false;
      std::string mode_str;
      if (!readStr(v, "mode", kMaxEnumLen, "bus", field("stops", i, "mode"), mode_str, err)) return false;
      if (!stringToMode(mode_str, s.mode)) {
        err = {"unknown mode \"" + mode_str + "\" (expected bus/trolley/subway/rail)", field("stops", i, "mode")};
        return false;
      }
      if (!readStr(v, "station", kMaxStopFieldLen, "", field("stops", i, "station"), s.station, err)) return false;
      if (!readStr(v, "direction", kMaxStopFieldLen, "", field("stops", i, "direction"), s.direction, err)) return false;
      if (!readStr(v, "headsign", kMaxStopFieldLen, "", field("stops", i, "headsign"), s.headsign, err)) return false;
      if (!readStr(v, "label", kMaxStopFieldLen, "", field("stops", i, "label"), s.label, err)) return false;
      if (!readStr(v, "stop_name", kMaxStopFieldLen, "", field("stops", i, "stop_name"), s.stop_name, err)) return false;
      if (!readInt(v, "show", 1, 4, 3, field("stops", i, "show"), n, err)) return false;
      s.show = (uint8_t)n;
      if (!readNum(v, "lat", -90, 90, 0.0, field("stops", i, "lat"), d, err)) return false;
      s.lat = d;
      if (!readNum(v, "lng", -180, 180, 0.0, field("stops", i, "lng"), d, err)) return false;
      s.lng = d;
      if (!readStr(v, "title_style", kMaxEnumLen, "label_dest", field("stops", i, "title_style"), s.title_style, err)) return false;
      if (!readStr(v, "title_text", kMaxStopFieldLen, "", field("stops", i, "title_text"), s.title_text, err)) return false;
      if (!readStr(v, "alt_of", kMaxStopFieldLen, "", field("stops", i, "alt_of"), s.alt_of, err)) return false;
      if (!readInt(v, "alt_after_min", 0, 255, 15, field("stops", i, "alt_after_min"), n, err)) return false;
      s.alt_after_min = (uint8_t)n;  // validateConfig() applies the 5-60 rule when alt_of is set
      if (s.mode == Mode::Rail) {
        // DESIGN.md SS6's rail example uses "line", not "route"; model.h's
        // StopConfig folds both into `route` (see its doc comment).
        if (!readStr(v, "line", kMaxStopFieldLen, "", field("stops", i, "route"), s.route, err)) return false;
        if (!normalizeRailLine(s.route, field("stops", i, "route"), err)) return false;
        s.stop_id = "";
      } else {
        // Bus/trolley/subway route ids are SEPTA's own strings ("17", "T4", "G1") and are stored
        // exactly as sent - only Regional Rail has a code-vs-display-name ambiguity to resolve.
        if (!readStr(v, "route", kMaxStopFieldLen, "", field("stops", i, "route"), s.route, err)) return false;
        if (!readStr(v, "stop_id", kMaxStopFieldLen, "", field("stops", i, "stop_id"), s.stop_id, err)) return false;
      }
      result.stops.push_back(s);
      ++i;
    }
  }

  result.alerts = doc["alerts"] | true;
  JsonVariantConst weather = doc["weather"];
  result.weather.enabled = weather["enabled"] | true;
  result.weather.per_stop = weather["per_stop"] | true;
  std::string units;
  if (!readStr(weather, "units", kMaxEnumLen, "f", "weather.units", units, err)) return false;
  if (units != "f" && units != "c") {
    err = {"weather.units must be \"f\" or \"c\"", "weather.units"};
    return false;
  }
  result.weather.fahrenheit = (units == "f");
  JsonVariantConst due = doc["due"];
  result.due.enabled = due["enabled"] | true;
  if (!readInt(due, "minutes", 1, 15, 3, "due.minutes", n, err)) return false;
  result.due.minutes = (uint8_t)n;
  result.due.led = due["led"] | true;
  result.due.screen = due["screen"] | true;
  result.due.chime = due["chime"] | false;

  JsonVariantConst profiles = doc["profiles"];
  if (profiles.is<JsonArrayConst>()) {
    JsonArrayConst parr = profiles.as<JsonArrayConst>();
    if (parr.size() > kMaxProfiles) {
      err = {"at most " + std::to_string(kMaxProfiles) + " profiles are allowed", "profiles"};
      return false;
    }
    size_t i = 0;
    for (JsonVariantConst pv : parr) {
      ProfileConfig p;
      if (!readStr(pv, "name", kMaxProfileNameLen, "", field("profiles", i, "name"), p.name, err)) return false;
      JsonVariantConst days = pv["days"];
      if (days.is<JsonArrayConst>()) {
        if (days.as<JsonArrayConst>().size() > kMaxDaysEntries) {
          err = {"days may list each of the 7 weekdays at most once", field("profiles", i, "days")};
          return false;
        }
        for (JsonVariantConst dv : days.as<JsonArrayConst>()) {
          int day = dv | -1;
          if (day >= 0 && day <= 6) p.days |= (uint8_t)(1u << day);
        }
      }
      if (!readStr(pv, "start", kMaxClockLen, "05:30", field("profiles", i, "start"), p.start, err)) return false;
      if (!readStr(pv, "end", kMaxClockLen, "10:00", field("profiles", i, "end"), p.end, err)) return false;
      JsonVariantConst ps = pv["stops"];
      if (ps.is<JsonArrayConst>()) {
        if (ps.as<JsonArrayConst>().size() > kMaxStops) {
          err = {"a profile may list at most " + std::to_string(kMaxStops) + " stops", field("profiles", i, "stops")};
          return false;
        }
        for (JsonVariantConst kv : ps.as<JsonArrayConst>()) {
          std::string key;
          if (!checkStr(kv, kMaxStopFieldLen, field("profiles", i, "stops"), key, err)) return false;
          // A duplicated key would draw the same panel twice and halve the space the other stops
          // get, so it is a 400 rather than something visibleStops() has to paper over.
          for (const std::string &seen : p.stops) {
            if (seen == key) {
              err = {"profile stop '" + key + "' is listed twice", field("profiles", i, "stops")};
              return false;
            }
          }
          p.stops.push_back(key);
        }
      }
      result.profiles.push_back(p);
      ++i;
    }
  }

  JsonVariantConst bike = doc["bike"];
  result.bike.enabled = bike["enabled"] | false;
  if (!readStr(bike, "style", kMaxEnumLen, "icons", "bike.style", result.bike.style, err)) return false;
  JsonVariantConst stations = bike["stations"];
  if (stations.is<JsonArrayConst>()) {
    JsonArrayConst sarr = stations.as<JsonArrayConst>();
    if (sarr.size() > kMaxBikeStations) {
      err = {"at most " + std::to_string(kMaxBikeStations) + " bike stations are allowed", "bike.stations"};
      return false;
    }
    size_t i = 0;
    for (JsonVariantConst sv : sarr) {
      BikeStation b;
      if (!readInt(sv, "id", 0, 2000000000, 0, field("bike.stations", i, "id"), n, err)) return false;
      b.id = (int)n;
      if (!readStr(sv, "name", kMaxBikeNameLen, "", field("bike.stations", i, "name"), b.name, err)) return false;
      result.bike.stations.push_back(b);
      ++i;
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

// ---------------------------------------------------------------------------
// Durable save (DESIGN.md SS6; review F09)
// ---------------------------------------------------------------------------
//
// The old saveConfig() truncated /config.json and streamed the new document straight into it.
// A power cut, a full filesystem or a short write between those two steps left a truncated file
// where the config used to be, and the next boot silently reverted the device to defaults - every
// stop the owner had added, gone. The sequence below never has the live file in a half-written
// state: everything lands in a temp file that is verified by reading it back and re-validating it
// before the old file is rotated to /config.prev.json and the temp file takes its place. If the
// power goes out in the middle, the worst case is that /config.json is missing and
// /config.prev.json is intact, which loadConfig() handles.
namespace {
SemaphoreHandle_t g_save_mutex = nullptr;
bool g_config_recovered = false;

SemaphoreHandle_t saveMutex() {
  if (g_save_mutex == nullptr) g_save_mutex = xSemaphoreCreateMutex();
  return g_save_mutex;
}

// Parses one file into `cfg`. Returns false (with a log line naming the reason) for missing,
// unreadable, malformed or invalid content.
bool readConfigFile(const char *path, Config &cfg) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) {
    log_e("config_store: failed to open %s for reading", path);
    return false;
  }
  JsonDocument doc;
  DeserializationError parse_err = deserializeJson(doc, f);
  f.close();
  if (parse_err) {
    log_e("config_store: %s is not valid JSON: %s", path, parse_err.c_str());
    return false;
  }
  ConfigError verr;
  Config parsed;
  if (!jsonToConfig(doc.as<JsonVariant>(), parsed, verr)) {
    log_e("config_store: %s failed validation at %s: %s", path, verr.path.c_str(), verr.message.c_str());
    return false;
  }
  cfg = parsed;
  return true;
}
}  // namespace

bool loadConfig(Config &cfg) {
  g_config_recovered = false;
  if (readConfigFile(kConfigPath, cfg)) {
    Serial.printf("[config] loaded %s\n", kConfigPath);
    return true;
  }
  if (readConfigFile(kConfigPrevPath, cfg)) {
    // Reported in GET /api/state as config_recovered so the web UI can tell the owner their last
    // save did not survive, instead of them noticing days later that a setting reverted.
    g_config_recovered = true;
    Serial.printf("[config] %s was unusable; recovered from %s\n", kConfigPath, kConfigPrevPath);
    log_w("config_store: recovered the previous config from %s", kConfigPrevPath);
    return true;
  }
  Serial.printf("[config] no usable config file\n");
  return false;
}

bool configRecovered() { return g_config_recovered; }

bool saveConfig(const Config &cfg) {
  JsonDocument doc;
  configToJson(cfg, doc);
  size_t want = measureJson(doc);
  if (want == 0) {
    log_e("config_store: serialized config measured 0 bytes");
    return false;
  }

  SemaphoreHandle_t mutex = saveMutex();
  // Serialized against itself: PUT /api/config runs on the async web server's task while
  // main.cpp can still be writing defaults, and two writers interleaving on one temp file would
  // produce exactly the corrupt file this function exists to avoid.
  if (mutex != nullptr && xSemaphoreTake(mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    log_e("config_store: timed out waiting for the save lock");
    return false;
  }
  bool ok = false;
  do {
    File f = LittleFS.open(kConfigTmpPath, "w");
    if (!f) {
      log_e("config_store: failed to open %s for writing", kConfigTmpPath);
      break;
    }
    size_t written = serializeJson(doc, f);
    f.close();
    if (written != want) {
      // A short write is what a full LittleFS looks like from here: the File API reports success
      // per chunk and simply stops accepting bytes. Anything but an exact match is a failure, so
      // PUT /api/config answers 500 and the live /config.json is left alone.
      log_e("config_store: short write to %s (%u of %u bytes)", kConfigTmpPath, (unsigned)written, (unsigned)want);
      LittleFS.remove(kConfigTmpPath);
      break;
    }
    Config verify;
    if (!readConfigFile(kConfigTmpPath, verify)) {
      log_e("config_store: %s did not read back as a valid config", kConfigTmpPath);
      LittleFS.remove(kConfigTmpPath);
      break;
    }
    if (LittleFS.exists(kConfigPath)) {
      LittleFS.remove(kConfigPrevPath);  // rename() will not overwrite an existing target
      if (!LittleFS.rename(kConfigPath, kConfigPrevPath)) {
        log_w("config_store: could not rotate %s to %s; saving anyway", kConfigPath, kConfigPrevPath);
        LittleFS.remove(kConfigPath);
      }
    }
    if (!LittleFS.rename(kConfigTmpPath, kConfigPath)) {
      log_e("config_store: could not rename %s to %s", kConfigTmpPath, kConfigPath);
      LittleFS.remove(kConfigTmpPath);
      break;
    }
    ok = true;
  } while (false);
  if (mutex != nullptr) xSemaphoreGive(mutex);
  return ok;
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
