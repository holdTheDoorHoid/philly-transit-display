#include "transit_core/septa.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "transit_core/timeparse.h"

namespace transit {

namespace {

// --- Tolerant JSON scalar extraction -----------------------------------------------------
// SEPTA's v1 API is inconsistent about quoting numeric fields (see septa.h's top comment); all
// four parsers below funnel every field through one of these instead of calling `.as<T>()`
// directly, so a field that unexpectedly changes from a JSON number to a JSON string (or vice
// versa) degrades gracefully instead of silently producing a zeroed/empty value.

std::string jsonToString(JsonVariantConst v) {
  if (v.isNull()) return std::string();
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s ? std::string(s) : std::string();
  }
  if (v.is<bool>()) return v.as<bool>() ? "true" : "false";
  if (v.is<long long>()) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%lld", v.as<long long>());
    return std::string(buf);
  }
  if (v.is<double>()) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v.as<double>());
    return std::string(buf);
  }
  return std::string();
}

int64_t jsonToInt64(JsonVariantConst v) {
  if (v.isNull()) return 0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s ? std::atoll(s) : 0;
  }
  if (v.is<long long>()) return v.as<long long>();
  if (v.is<double>()) return static_cast<int64_t>(v.as<double>());
  if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
  return 0;
}

double jsonToDouble(JsonVariantConst v) {
  if (v.isNull()) return 0.0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s ? std::atof(s) : 0.0;
  }
  if (v.is<double>()) return v.as<double>();
  if (v.is<long long>()) return static_cast<double>(v.as<long long>());
  return 0.0;
}

// Returns the SEPTA `{"error": "..."}` message if present, else an empty string. Every
// endpoint's error shape observed in practice is a plain top-level "error" string field.
std::string sepptaErrorMessage(JsonVariantConst root) {
  if (!root.is<JsonObjectConst>()) return std::string();
  JsonVariantConst e = root["error"];
  if (e.isNull() || !e.is<const char*>()) return std::string();
  return jsonToString(e);
}

// --- Alert text cleanup ---------------------------------------------------------------------

std::string trimWs(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string decodeHtmlEntities(const std::string& s) {
  static const struct {
    const char* name;
    char ch;
  } kEntities[] = {{"&nbsp;", ' '}, {"&amp;", '&'},  {"&lt;", '<'},
                    {"&gt;", '>'},  {"&quot;", '"'}, {"&apos;", '\''},
                    {"&#39;", '\''}};
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      bool matched = false;
      for (const auto& e : kEntities) {
        size_t l = std::strlen(e.name);
        if (s.compare(i, l, e.name) == 0) {
          out.push_back(e.ch);
          i += l;
          matched = true;
          break;
        }
      }
      if (!matched) {
        out.push_back(s[i]);
        ++i;
      }
    } else {
      out.push_back(s[i]);
      ++i;
    }
  }
  return out;
}

std::string collapseWhitespace(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool in_ws = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!in_ws) {
        out.push_back(' ');
        in_ws = true;
      }
    } else {
      out.push_back(c);
      in_ws = false;
    }
  }
  return out;
}

// Strips HTML tags, decodes a handful of common entities, collapses whitespace, trims, and caps
// the result to `max_len` characters (cutting at the last space at-or-before the limit so a word
// is not sliced in half, when that space is not implausibly early in the string).
std::string stripHtmlAndTrim(const std::string& raw, size_t max_len) {
  std::string no_tags;
  no_tags.reserve(raw.size());
  bool in_tag = false;
  for (char c : raw) {
    if (c == '<') {
      in_tag = true;
      continue;
    }
    if (c == '>') {
      in_tag = false;
      continue;
    }
    if (!in_tag) no_tags.push_back(c);
  }
  std::string trimmed = trimWs(collapseWhitespace(decodeHtmlEntities(no_tags)));
  if (trimmed.size() <= max_len) return trimmed;
  size_t cut = trimmed.rfind(' ', max_len);
  if (cut == std::string::npos || cut < max_len / 2) cut = max_len;
  return trimmed.substr(0, cut);
}

}  // namespace

ParseResult<TvVehicle> parseTransitView(const uint8_t* data, size_t len) {
  ParseResult<TvVehicle> result;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    result.ok = false;
    result.error = std::string("json parse error: ") + err.c_str();
    return result;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  JsonArrayConst arr;
  if (root.is<JsonArrayConst>()) {
    arr = root.as<JsonArrayConst>();  // bare [] - unrecognized route or nothing tracked right now
  } else if (root.is<JsonObjectConst>()) {
    std::string em = sepptaErrorMessage(root);
    if (!em.empty()) {
      result.ok = false;
      result.error = em;
      return result;
    }
    JsonVariantConst bus = root["bus"];
    if (!bus.is<JsonArrayConst>()) {
      result.ok = false;
      result.error = "unexpected TransitView response shape";
      return result;
    }
    arr = bus.as<JsonArrayConst>();
  } else {
    result.ok = false;
    result.error = "unexpected TransitView response shape";
    return result;
  }

  for (JsonObjectConst v : arr) {
    TvVehicle tv;
    tv.trip = jsonToString(v["trip"]);
    tv.vehicle_id = jsonToString(v["VehicleID"]);
    tv.late = static_cast<int>(jsonToInt64(v["late"]));
    tv.destination = jsonToString(v["destination"]);
    tv.direction = jsonToString(v["Direction"]);
    tv.next_stop_id = jsonToString(v["next_stop_id"]);
    tv.next_stop_sequence = static_cast<uint32_t>(jsonToInt64(v["next_stop_sequence"]));
    tv.seats = jsonToString(v["estimated_seat_availability"]);
    tv.timestamp = jsonToInt64(v["timestamp"]);
    tv.lat = jsonToDouble(v["lat"]);
    tv.lng = jsonToDouble(v["lng"]);
    result.items.push_back(std::move(tv));
  }
  return result;
}

ParseResult<SchedEntry> parseBusSchedules(const uint8_t* data, size_t len) {
  ParseResult<SchedEntry> result;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    result.ok = false;
    result.error = std::string("json parse error: ") + err.c_str();
    return result;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!root.is<JsonObjectConst>()) {
    result.ok = false;
    result.error = "unexpected BusSchedules response shape";
    return result;
  }
  std::string em = sepptaErrorMessage(root);
  if (!em.empty()) {
    result.ok = false;
    result.error = em;
    return result;
  }

  for (JsonPairConst kv : root.as<JsonObjectConst>()) {
    std::string route = kv.key().c_str();
    JsonVariantConst val = kv.value();
    if (!val.is<JsonArrayConst>()) continue;
    for (JsonObjectConst e : val.as<JsonArrayConst>()) {
      SchedEntry se;
      se.route = route;
      se.trip_id = jsonToString(e["trip_id"]);
      se.scheduled = parseBusScheduleTime(jsonToString(e["DateCalender"]));
      se.direction = jsonToString(e["Direction"]);
      se.direction_desc = jsonToString(e["DirectionDesc"]);
      se.stop_name = jsonToString(e["StopName"]);
      result.items.push_back(std::move(se));
    }
  }
  return result;
}

ParseResult<transit::Alert> parseAlerts(const uint8_t* data, size_t len) {
  ParseResult<transit::Alert> result;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    result.ok = false;
    result.error = std::string("json parse error: ") + err.c_str();
    return result;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!root.is<JsonArrayConst>()) {
    result.ok = false;
    result.error = "unexpected Alerts response shape";
    return result;
  }

  for (JsonObjectConst e : root.as<JsonArrayConst>()) {
    transit::Alert a;
    a.route = jsonToString(e["route"]);

    std::string advisory = jsonToString(e["advisory"]);
    std::string alert_field = jsonToString(e["alert"]);
    std::string description = jsonToString(e["description"]);
    const std::string& raw =
        !advisory.empty() ? advisory : (!alert_field.empty() ? alert_field : description);
    a.text = stripHtmlAndTrim(raw, 240);

    JsonVariantConst detour = e["detour"];
    if (detour.is<JsonArrayConst>()) {
      for (JsonObjectConst d : detour.as<JsonArrayConst>()) {
        std::string reason = trimWs(jsonToString(d["reason"]));
        std::string message = trimWs(jsonToString(d["message"]));
        std::string summary = reason.empty() ? message : (reason + ": " + message);
        if (!summary.empty()) a.detours.push_back(summary);
      }
    }

    // This endpoint doesn't publish an explicit "still active" flag distinct from simply being
    // in the response at all; every alert returned is treated as current (see septa.h).
    a.current = true;
    result.items.push_back(std::move(a));
  }
  return result;
}

ParseResult<RailArrival> parseRailArrivals(const uint8_t* data, size_t len) {
  ParseResult<RailArrival> result;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    result.ok = false;
    result.error = std::string("json parse error: ") + err.c_str();
    return result;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!root.is<JsonObjectConst>()) {
    result.ok = false;
    result.error = "unexpected Arrivals response shape";
    return result;
  }
  std::string em = sepptaErrorMessage(root);
  if (!em.empty()) {
    result.ok = false;
    result.error = em;
    return result;
  }

  JsonArrayConst groups;
  bool found = false;
  for (JsonPairConst kv : root.as<JsonObjectConst>()) {
    JsonVariantConst v = kv.value();
    if (v.is<JsonArrayConst>()) {
      groups = v.as<JsonArrayConst>();
      found = true;
      break;
    }
  }
  if (!found) {
    result.ok = false;
    result.error = "unexpected Arrivals response shape";
    return result;
  }

  for (JsonObjectConst grp : groups) {
    for (JsonPairConst dkv : grp) {
      std::string key = dkv.key().c_str();
      std::string dir;
      if (key.find("North") != std::string::npos) {
        dir = "N";
      } else if (key.find("South") != std::string::npos) {
        dir = "S";
      } else {
        dir = key;  // unrecognized grouping name; preserved as-is rather than dropped
      }
      JsonVariantConst trains = dkv.value();
      if (!trains.is<JsonArrayConst>()) continue;
      for (JsonObjectConst t : trains.as<JsonArrayConst>()) {
        RailArrival ra;
        ra.direction = dir;
        ra.train_id = jsonToString(t["train_id"]);
        ra.line = jsonToString(t["line"]);
        ra.destination = jsonToString(t["destination"]);
        ra.origin = jsonToString(t["origin"]);
        ra.status = jsonToString(t["status"]);
        ra.sched = parseArrivalsTime(jsonToString(t["sched_time"]));
        ra.depart = parseArrivalsTime(jsonToString(t["depart_time"]));
        ra.track = jsonToString(t["track"]);
        ra.next_station = jsonToString(t["next_station"]);
        result.items.push_back(std::move(ra));
      }
    }
  }
  return result;
}

const RailLine kRailLines[] = {
    {"AIR", "Airport", "apt"},
    {"CHE", "Chestnut Hill East", "che"},
    {"CHW", "Chestnut Hill West", "chw"},
    {"CYN", "Cynwyd", "cyn"},
    {"FOX", "Fox Chase", "fxc"},
    {"LAN", "Lansdale/Doylestown", "landdoy"},
    {"MED", "Media/Wawa", "med"},
    {"NOR", "Manayunk/Norristown", "nor"},
    {"PAO", "Paoli/Thorndale", "pao"},
    {"TRE", "Trenton", "trent"},
    {"WAR", "Warminster", "warm"},
    {"WIL", "Wilmington/Newark", "wilm"},
    {"WTR", "West Trenton", "wtren"},
};
const size_t kRailLineCount = sizeof(kRailLines) / sizeof(kRailLines[0]);

const RailLine* findRailLine(const std::string& code) {
  for (size_t i = 0; i < kRailLineCount; ++i) {
    if (code == kRailLines[i].code) return &kRailLines[i];
  }
  return nullptr;
}

}  // namespace transit
