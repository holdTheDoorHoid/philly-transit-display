#include "weather_core/weather.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>

namespace weather {

Kind kindForCode(int c) {
  if (c == 0 || c == 1) return Kind::Clear;
  if (c == 2 || c == 3) return Kind::Cloudy;
  if (c == 45 || c == 48) return Kind::Fog;
  if (c >= 51 && c <= 55) return Kind::Drizzle;
  if (c == 56 || c == 57 || c == 66 || c == 67) return Kind::FreezingRain;
  if (c >= 61 && c <= 65) return Kind::Rain;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return Kind::Snow;
  if (c >= 80 && c <= 82) return Kind::Showers;
  if (c >= 95 && c <= 99) return Kind::Thunder;
  return Kind::Unknown;
}

const char* codeText(int c) {
  switch (c) {
    case 0: return "clear";
    case 1: return "mostly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "fog";
    case 51: case 53: return "drizzle";
    case 55: return "heavy drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66: case 67: return "freezing rain";
    case 71: return "light snow";
    case 73: return "snow";
    case 75: return "heavy snow";
    case 77: return "snow grains";
    case 80: return "light showers";
    case 81: return "showers";
    case 82: return "heavy showers";
    case 85: return "snow showers";
    case 86: return "heavy snow showers";
    case 95: return "thunderstorm";
    case 96: case 99: return "thunderstorm, hail";
    default: return "";
  }
}

bool isWet(Kind k) {
  switch (k) {
    case Kind::Drizzle: case Kind::Rain: case Kind::FreezingRain: case Kind::Snow: case Kind::Showers: case Kind::Thunder:
      return true;
    default:
      return false;
  }
}

const Hour* Forecast::at(int64_t epoch) const {
  for (const Hour& h : hours) {
    if (epoch >= h.epoch && epoch < h.epoch + 3600) return &h;
  }
  return nullptr;
}

std::string openMeteoUrl(double lat, double lng, bool fahrenheit, int forecast_hours) {
  if (forecast_hours < 1) forecast_hours = 1;
  if (forecast_hours > 24) forecast_hours = 24;
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m"
                "&hourly=weather_code,precipitation_probability,temperature_2m"
                "&forecast_hours=%d&timezone=auto&wind_speed_unit=mph%s",
                lat, lng, forecast_hours, fahrenheit ? "&temperature_unit=fahrenheit" : "");
  return buf;
}

namespace {

int64_t daysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int yoe = static_cast<int>(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

}  // namespace

int64_t parseIsoLocal(const std::string& s, int32_t utc_offset_s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
  int64_t naive = daysFromCivil(y, mo, d) * 86400 + static_cast<int64_t>(h) * 3600 + static_cast<int64_t>(mi) * 60;
  return naive - utc_offset_s;
}

bool parseOpenMeteo(const uint8_t* data, size_t len, Forecast* out, std::string* err) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, data, len);
  if (e) {
    if (err) *err = std::string("json parse error: ") + e.c_str();
    return false;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    if (err) *err = "unexpected shape";
    return false;
  }
  if (root["error"].is<bool>() && root["error"].as<bool>()) {
    if (err) *err = std::string(root["reason"] | "open-meteo error");
    return false;
  }
  JsonObjectConst cur = root["current"];
  if (cur.isNull() || !cur["weather_code"].is<int>()) {
    if (err) *err = "no current block";
    return false;
  }
  Forecast f;
  f.utc_offset_s = root["utc_offset_seconds"] | 0;
  f.observed = parseIsoLocal(std::string(cur["time"] | ""), f.utc_offset_s);
  f.temp = cur["temperature_2m"] | 0.0f;
  f.feels_like = cur["apparent_temperature"] | f.temp;
  f.code = cur["weather_code"] | -1;
  f.wind = cur["wind_speed_10m"] | 0.0f;

  JsonObjectConst hourly = root["hourly"];
  if (!hourly.isNull()) {
    JsonArrayConst times = hourly["time"];
    JsonArrayConst codes = hourly["weather_code"];
    JsonArrayConst probs = hourly["precipitation_probability"];
    JsonArrayConst temps = hourly["temperature_2m"];
    size_t n = times.size();
    for (size_t i = 0; i < n && i < 24; ++i) {
      Hour h;
      h.epoch = parseIsoLocal(std::string(times[i] | ""), f.utc_offset_s);
      if (h.epoch == 0) continue;
      h.code = i < codes.size() ? (codes[i] | -1) : -1;
      h.precip_prob = i < probs.size() ? (probs[i] | -1) : -1;
      h.temp = i < temps.size() ? (temps[i] | 0.0f) : 0.0f;
      f.hours.push_back(h);
    }
  }
  *out = std::move(f);
  return true;
}

bool notable(const Hour& h, int baseline_code) {
  if (h.code < 0) return false;
  Kind k = kindForCode(h.code);
  if (h.precip_prob >= 40) return true;
  if (isWet(k)) return true;
  Kind base = kindForCode(baseline_code);
  if (base == Kind::Unknown) return false;
  // Clear vs cloudy is not worth a line; fog, or anything wet, is.
  bool base_dry_sky = (base == Kind::Clear || base == Kind::Cloudy);
  bool dry_sky = (k == Kind::Clear || k == Kind::Cloudy);
  if (base_dry_sky && dry_sky) return false;
  return k != base;
}

bool nearby(double lat1, double lng1, double lat2, double lng2, double km) {
  const double kKmPerDegLat = 111.2;
  double dlat = (lat1 - lat2) * kKmPerDegLat;
  double dlng = (lng1 - lng2) * kKmPerDegLat * std::cos((lat1 + lat2) * 0.5 * 3.14159265358979 / 180.0);
  return dlat * dlat + dlng * dlng <= km * km;
}

}  // namespace weather
