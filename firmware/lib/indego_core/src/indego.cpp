#include "indego_core/indego.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace indego {

namespace {

// Extracts the value of a `"id":<n>` pair (optional whitespace after the colon) from raw JSON
// text. Cheap enough to run before deciding whether a feature is worth a full ArduinoJson parse.
bool extractId(const std::vector<uint8_t>& buf, int* out) {
  static const char kKey[] = "\"id\":";
  constexpr size_t kKeyLen = sizeof(kKey) - 1;  // exclude the trailing NUL
  if (buf.size() < kKeyLen) return false;
  for (size_t i = 0; i + kKeyLen <= buf.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < kKeyLen; ++j) {
      if (buf[i + j] != static_cast<uint8_t>(kKey[j])) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    size_t p = i + kKeyLen;
    while (p < buf.size() && (buf[p] == ' ' || buf[p] == '\t' || buf[p] == '\n' || buf[p] == '\r')) ++p;
    bool neg = false;
    if (p < buf.size() && buf[p] == '-') {
      neg = true;
      ++p;
    }
    if (p >= buf.size() || buf[p] < '0' || buf[p] > '9') return false;
    long val = 0;
    while (p < buf.size() && buf[p] >= '0' && buf[p] <= '9') {
      val = val * 10 + (buf[p] - '0');
      ++p;
    }
    *out = static_cast<int>(neg ? -val : val);
    return true;
  }
  return false;
}

// Parses one complete feature object (raw bytes from '{' to '}') into a Station, using an
// ArduinoJson filter limited to the fields Station needs so the parse itself stays small (no
// "geometry", no per-dock "bikes" array).
bool parseStation(const std::vector<uint8_t>& buf, Station* out) {
  JsonDocument filter;
  JsonObject props = filter["properties"].to<JsonObject>();
  props["id"] = true;
  props["name"] = true;
  props["bikesAvailable"] = true;
  props["electricBikesAvailable"] = true;
  props["classicBikesAvailable"] = true;
  props["docksAvailable"] = true;
  props["totalDocks"] = true;
  props["kioskPublicStatus"] = true;

  JsonDocument doc;
  DeserializationError e =
      deserializeJson(doc, buf.data(), buf.size(), DeserializationOption::Filter(filter));
  if (e) return false;

  JsonObjectConst root = doc.as<JsonObjectConst>();
  JsonObjectConst p = root["properties"];
  if (p.isNull()) return false;

  Station st;
  st.id = p["id"] | 0;
  st.name = std::string(p["name"] | "");
  st.bikes = p["bikesAvailable"] | -1;
  st.ebikes = p["electricBikesAvailable"] | -1;
  st.classic = p["classicBikesAvailable"] | -1;
  st.docks = p["docksAvailable"] | -1;
  st.total_docks = p["totalDocks"] | -1;
  st.active = std::string(p["kioskPublicStatus"] | "") == "Active";
  *out = std::move(st);
  return true;
}

}  // namespace

void StatusStream::setStationFilter(const std::vector<int>& ids) {
  filter_ids_ = ids;
  remaining_ids_ = ids;
}

void StatusStream::appendFeatureByte(uint8_t b) {
  if (feature_buf_.size() < kMaxFeatureBytes) {
    feature_buf_.push_back(b);
  } else {
    feature_oversized_ = true;
  }
}

void StatusStream::onFeatureClosed() {
  ++features_seen_;
  bool oversized = feature_oversized_;
  feature_oversized_ = false;

  if (oversized) {
    ++features_skipped_;
    feature_buf_.clear();
    return;
  }
  if (filter_ids_.empty()) {
    feature_buf_.clear();
    return;
  }

  int id = 0;
  if (extractId(feature_buf_, &id)) {
    auto it = std::find(remaining_ids_.begin(), remaining_ids_.end(), id);
    if (it != remaining_ids_.end()) {
      Station st;
      if (parseStation(feature_buf_, &st)) {
        stations_.push_back(std::move(st));
      }
      remaining_ids_.erase(it);
      if (remaining_ids_.empty()) done_ = true;
    }
  }
  feature_buf_.clear();
}

void StatusStream::processByte(uint8_t b) {
  switch (state_) {
    case State::kSeekKey: {
      static const char kKey[] = "\"features\"";
      constexpr size_t kKeyLen = sizeof(kKey) - 1;
      if (b == static_cast<uint8_t>(kKey[key_match_pos_])) {
        ++key_match_pos_;
        if (key_match_pos_ == kKeyLen) {
          key_match_pos_ = 0;
          state_ = State::kSeekColon;
        }
      } else {
        key_match_pos_ = (b == static_cast<uint8_t>(kKey[0])) ? 1 : 0;
      }
      break;
    }
    case State::kSeekColon:
      if (b == ':') state_ = State::kSeekBracket;
      break;
    case State::kSeekBracket:
      if (b == '[') state_ = State::kBetweenFeatures;
      break;
    case State::kBetweenFeatures:
      if (b == '{') {
        depth_ = 1;
        in_string_ = false;
        escape_ = false;
        feature_oversized_ = false;
        feature_buf_.clear();
        appendFeatureByte(b);
        state_ = State::kInFeature;
      } else if (b == ']') {
        state_ = State::kIgnoreRest;
      }
      break;
    case State::kInFeature:
      if (in_string_) {
        appendFeatureByte(b);
        if (escape_) {
          escape_ = false;
        } else if (b == '\\') {
          escape_ = true;
        } else if (b == '"') {
          in_string_ = false;
        }
      } else {
        if (b == '"') {
          in_string_ = true;
          appendFeatureByte(b);
        } else if (b == '{') {
          ++depth_;
          appendFeatureByte(b);
        } else if (b == '}') {
          --depth_;
          appendFeatureByte(b);
          if (depth_ == 0) {
            onFeatureClosed();
            state_ = State::kBetweenFeatures;
          }
        } else {
          appendFeatureByte(b);
        }
      }
      break;
    case State::kIgnoreRest:
      break;
  }
}

bool StatusStream::push(const uint8_t* data, size_t len) {
  if (done_) return false;
  for (size_t i = 0; i < len; ++i) {
    processByte(data[i]);
    if (done_) return false;
  }
  return true;
}

void StatusStream::finish() { done_ = true; }

std::string statusUrl() { return "http://bts-status.bicycletransit.workers.dev/phl"; }

}  // namespace indego
