// Weather for the header and the stop panels (DESIGN.md 4.8): Open-Meteo forecast fetch + parse
// and the WMO weather-code vocabulary. Arduino-independent: compiles on the host (native tests)
// and on ESP32. Open-Meteo was chosen because it needs no API key, answers over plain HTTP (the
// classic ESP32 cannot afford TLS alongside LVGL and Wi-Fi - http_fetch.h), and a current +
// six-hour request is under 1 KB.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace weather {

// Coarse categories of the WMO interpretation codes Open-Meteo returns.
enum class Kind : uint8_t { Clear, Cloudy, Fog, Drizzle, Rain, FreezingRain, Snow, Showers, Thunder, Unknown };

Kind kindForCode(int wmo_code);
// Short lower-case wording for the header/panels: "clear", "partly cloudy", "rain", "storms"...
const char* codeText(int wmo_code);
// Rain/drizzle/snow/storms: weather the rider would want an umbrella for.
bool isWet(Kind k);

struct Hour {
  int64_t epoch = 0;      // start of the hour, UTC epoch seconds
  int code = -1;          // WMO code
  int precip_prob = -1;   // percent, -1 if absent
  float temp = 0;         // in the requested unit
};

struct Forecast {
  int64_t observed = 0;   // "current.time" as an epoch, 0 if absent
  int32_t utc_offset_s = 0;
  float temp = 0;
  float feels_like = 0;
  int code = -1;          // current WMO code, -1 = no data
  float wind = 0;
  std::vector<Hour> hours;  // ascending
  bool valid() const { return code >= 0; }
  // The hourly slot covering `epoch`, or nullptr if the forecast doesn't reach it.
  const Hour* at(int64_t epoch) const;
};

// forecast_hours <= 24; Open-Meteo caps the hourly arrays to that many entries starting at the
// current hour.
std::string openMeteoUrl(double lat, double lng, bool fahrenheit, int forecast_hours);

// Parses an Open-Meteo /v1/forecast body built by openMeteoUrl(). Returns false with `err` set
// on a malformed or error-shaped body; `out` is only written on success.
bool parseOpenMeteo(const uint8_t* data, size_t len, Forecast* out, std::string* err);

// "2026-09-14T09:45" (local wall clock) + the response's utc_offset_seconds -> UTC epoch.
// Returns 0 if the string doesn't parse.
int64_t parseIsoLocal(const std::string& s, int32_t utc_offset_s);

// Whether the hour is worth calling out next to a stop when the header already shows the main
// location's current conditions: precipitation likely (>= 40 %), or a wet kind of weather, or a
// different kind than `baseline_code`'s (fog vs clear, snow vs rain...).
bool notable(const Hour& h, int baseline_code);

// Two coordinates within roughly `km` of each other (equirectangular approximation, fine for a
// city): used to share one forecast between nearby stops.
bool nearby(double lat1, double lng1, double lat2, double lng2, double km);

}  // namespace weather
