#include "transit_core/septa.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "transit_core/numparse.h"
#include "transit_core/timeparse.h"

namespace transit {

namespace {

// --- Tolerant JSON scalar extraction -----------------------------------------------------
// SEPTA's v1 API is inconsistent about quoting numeric fields (see septa.h's top comment); all
// four parsers below funnel every field through one of these instead of calling `.as<T>()`
// directly, so a field that unexpectedly changes from a JSON number to a JSON string (or vice
// versa) degrades gracefully instead of silently producing a zeroed/empty value.

// Truncates an agency-supplied identifier/label to kMaxIdChars. Every string these parsers keep
// goes through here: the retention caps above bound the NUMBER of items, this bounds the size of
// each one, so a response with a megabyte-long "destination" cannot grow the heap either.
std::string capId(std::string s) {
  if (s.size() > kMaxIdChars) s.resize(kMaxIdChars);
  return s;
}

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

// Quoted-number fields go through parseIntStrict(), never atoll(): atoll() is undefined
// behaviour on an out-of-range input, and "late": "99999999999999999999" is one HTTP response
// away at any time (numparse.h). A value that doesn't parse, or doesn't fit, reads as 0 - the
// same as a missing field, which every caller already handles.
int64_t jsonToInt64(JsonVariantConst v) {
  if (v.isNull()) return 0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s) return 0;
    int64_t out = 0;
    if (!parseIntStrict(std::string(s), kMaxSafeDigits, INT64_MIN, INT64_MAX, &out)) return 0;
    return out;
  }
  if (v.is<long long>()) return v.as<long long>();
  if (v.is<double>()) {
    // A JSON number big enough to be out of int64 range makes the cast UB, so clamp first.
    double d = v.as<double>();
    if (!(d > -9.2e18 && d < 9.2e18)) return 0;
    return static_cast<int64_t>(d);
  }
  if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
  return 0;
}

double jsonToDouble(JsonVariantConst v) {
  if (v.isNull()) return 0.0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s) return 0.0;
    double out = 0.0;
    if (!parseDoubleStrict(std::string(s), &out)) return 0.0;  // strtod, not atof (numparse.h)
    return out;
  }
  if (v.is<double>()) return v.as<double>();
  if (v.is<long long>()) return static_cast<double>(v.as<long long>());
  return 0.0;
}

// --- Bounded retention -----------------------------------------------------------------------
// Appends `item` to `out` while `out` holds fewer than `cap` items. Once full, the item whose
// `key` is FARTHEST in the future is evicted in favour of a nearer one; a key of 0 ("unknown
// time", e.g. an unparseable DateCalender) sorts as farthest so those go first. That keeps the
// soonest arrivals, which is the only part of a transit feed a display can use, and means the
// vector never grows past its single reserve() - no reallocation, no unbounded growth.
// `dropped` counts every item that did not survive.
// `keyOf(item)` returns the time to rank an already-retained item by.
template <typename T, typename KeyFn>
void keepNearest(std::vector<T>* out, size_t cap, Epoch key, T&& item, uint32_t* dropped,
                  KeyFn keyOf) {
  if (out->size() < cap) {
    out->push_back(std::move(item));
    return;
  }
  ++*dropped;
  if (cap == 0) return;
  size_t worst = 0;
  Epoch worst_key = keyOf((*out)[0]);
  bool worst_unknown = (worst_key == 0);
  for (size_t i = 1; i < out->size(); ++i) {
    Epoch k = keyOf((*out)[i]);
    bool unknown = (k == 0);
    if ((unknown && !worst_unknown) || (unknown == worst_unknown && k > worst_key)) {
      worst = i;
      worst_key = k;
      worst_unknown = unknown;
    }
  }
  bool item_unknown = (key == 0);
  bool nearer = worst_unknown ? !item_unknown : (!item_unknown && key < worst_key);
  if (nearer) (*out)[worst] = std::move(item);
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

// ASCII case-insensitive compare against a C string literal from kRailLines.
bool equalsIgnoreCaseC(const std::string& a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return i == a.size() && b[i] == '\0';
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

  // A vehicle list has no "nearest" ordering to prefer (these are positions, not arrival times),
  // so the cap keeps the first kMaxTvVehicles in wire order and counts the rest.
  result.items.reserve(kMaxTvVehicles);
  for (JsonObjectConst v : arr) {
    if (result.items.size() >= kMaxTvVehicles) {
      ++result.dropped;
      continue;
    }
    TvVehicle tv;
    tv.trip = capId(jsonToString(v["trip"]));
    tv.vehicle_id = capId(jsonToString(v["VehicleID"]));
    tv.late = static_cast<int>(jsonToInt64(v["late"]));
    tv.destination = capId(jsonToString(v["destination"]));
    tv.direction = capId(jsonToString(v["Direction"]));
    tv.next_stop_id = capId(jsonToString(v["next_stop_id"]));
    tv.next_stop_sequence = static_cast<uint32_t>(jsonToInt64(v["next_stop_sequence"]));
    tv.seats = capId(jsonToString(v["estimated_seat_availability"]));
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

  result.items.reserve(kMaxSchedEntries);
  for (JsonPairConst kv : root.as<JsonObjectConst>()) {
    std::string route = capId(kv.key().c_str());
    JsonVariantConst val = kv.value();
    if (!val.is<JsonArrayConst>()) continue;
    for (JsonObjectConst e : val.as<JsonArrayConst>()) {
      SchedEntry se;
      se.route = route;
      se.trip_id = capId(jsonToString(e["trip_id"]));
      se.scheduled = parseBusScheduleTime(jsonToString(e["DateCalender"]));
      se.direction = capId(jsonToString(e["Direction"]));
      se.direction_desc = capId(jsonToString(e["DirectionDesc"]));
      se.stop_name = capId(jsonToString(e["StopName"]));
      // Keep the kMaxSchedEntries soonest entries; a real response is 4-12, so this only bites
      // on a pathological body (and then the far-future rows are the ones a display can spare).
      Epoch rank = se.scheduled;
      keepNearest(&result.items, kMaxSchedEntries, rank, std::move(se), &result.dropped,
                   [](const SchedEntry& x) { return x.scheduled; });
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

  result.items.reserve(kMaxAlerts);
  for (JsonObjectConst e : root.as<JsonArrayConst>()) {
    if (result.items.size() >= kMaxAlerts) {
      ++result.dropped;
      continue;
    }
    transit::Alert a;
    a.route = capId(jsonToString(e["route"]));

    std::string advisory = jsonToString(e["advisory"]);
    std::string alert_field = jsonToString(e["alert"]);
    std::string description = jsonToString(e["description"]);
    const std::string& raw =
        !advisory.empty() ? advisory : (!alert_field.empty() ? alert_field : description);
    a.text = stripHtmlAndTrim(raw, 240);

    JsonVariantConst detour = e["detour"];
    if (detour.is<JsonArrayConst>()) {
      constexpr size_t kMaxDetours = 8;  // the ticker can only show a couple (DESIGN.md 8)
      a.detours.reserve(kMaxDetours);
      for (JsonObjectConst d : detour.as<JsonArrayConst>()) {
        if (a.detours.size() >= kMaxDetours) {
          ++result.dropped;
          continue;
        }
        std::string reason = trimWs(jsonToString(d["reason"]));
        std::string message = trimWs(jsonToString(d["message"]));
        std::string summary = reason.empty() ? message : (reason + ": " + message);
        if (summary.size() > 240) summary.resize(240);  // same bound as Alert::text
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

  result.items.reserve(kMaxRailArrivals);
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
        ra.train_id = capId(jsonToString(t["train_id"]));
        ra.line = capId(jsonToString(t["line"]));
        ra.destination = capId(jsonToString(t["destination"]));
        ra.origin = capId(jsonToString(t["origin"]));
        ra.status = capId(jsonToString(t["status"]));
        ra.sched = parseArrivalsTime(jsonToString(t["sched_time"]));
        ra.depart = parseArrivalsTime(jsonToString(t["depart_time"]));
        ra.track = capId(jsonToString(t["track"]));
        ra.next_station = capId(jsonToString(t["next_station"]));
        // Nearest-first, on the same reasoning as BusSchedules: `results=5` per direction is what
        // we ask for, so the cap is only reached if SEPTA answers with far more than requested.
        Epoch rank = ra.depart ? ra.depart : ra.sched;
        keepNearest(&result.items, kMaxRailArrivals, rank, std::move(ra), &result.dropped,
                     [](const RailArrival& x) { return x.depart ? x.depart : x.sched; });
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

const RailLine* findRailLineByName(const std::string& code_or_display_name) {
  if (code_or_display_name.empty()) return nullptr;
  for (size_t i = 0; i < kRailLineCount; ++i) {
    if (equalsIgnoreCaseC(code_or_display_name, kRailLines[i].code) ||
        equalsIgnoreCaseC(code_or_display_name, kRailLines[i].display_name)) {
      return &kRailLines[i];
    }
  }
  return nullptr;
}

}  // namespace transit
