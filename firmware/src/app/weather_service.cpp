#include "weather_service.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

namespace transit_app {

namespace {

constexpr uint32_t kRefreshMs = 10 * 60 * 1000;
constexpr size_t kMaxLocations = 4;
constexpr double kShareKm = 1.5;
constexpr int kForecastHours = 6;
constexpr size_t kBodyCap = 3072;  // a 6-hour response is ~900 B

struct Location {
  double lat = 0;
  double lng = 0;
  weather::Forecast forecast;
  uint32_t fetched_ms = 0;
  bool ok = false;
};

struct StopLocation {
  std::string key;
  int location = -1;  // index into g_locations, -1 = use main (index 0)
};

SemaphoreHandle_t g_mutex = nullptr;
std::vector<Location> g_locations;
std::vector<StopLocation> g_stops;
bool g_enabled = false;
bool g_per_stop = true;
bool g_fahrenheit = true;
uint32_t g_fetched_epoch = 0;
volatile bool g_invalidate = true;
std::string g_fingerprint;  // stop coordinates the current grouping was built from

std::string stopsFingerprint(const Config &cfg) {
  char buf[40];
  std::string fp;
  for (const transit::StopConfig &s : cfg.stops) {
    snprintf(buf, sizeof(buf), "%s:%.4f,%.4f;", s.key.c_str(), s.lat, s.lng);
    fp += buf;
  }
  return fp;
}

SemaphoreHandle_t mutex() {
  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
  return g_mutex;
}

struct Lock {
  explicit Lock(uint32_t ms = 500) : held(xSemaphoreTake(mutex(), pdMS_TO_TICKS(ms)) == pdTRUE) {}
  ~Lock() {
    if (held) xSemaphoreGive(mutex());
  }
  bool held;
};

// Groups the configured stops by location. Stops without coordinates map to the main location.
void regroup(const Config &cfg, std::vector<Location> &locs, std::vector<StopLocation> &stops) {
  locs.clear();
  stops.clear();
  for (const transit::StopConfig &s : cfg.stops) {
    StopLocation sl;
    sl.key = s.key;
    if (s.lat != 0 || s.lng != 0) {
      for (size_t i = 0; i < locs.size(); ++i) {
        if (weather::nearby(locs[i].lat, locs[i].lng, s.lat, s.lng, kShareKm)) {
          sl.location = (int)i;
          break;
        }
      }
      if (sl.location < 0 && (locs.empty() || cfg.weather.per_stop) && locs.size() < kMaxLocations) {
        Location l;
        l.lat = s.lat;
        l.lng = s.lng;
        locs.push_back(l);
        sl.location = (int)locs.size() - 1;
      }
    }
    stops.push_back(sl);
  }
}

bool fetchLocation(Location &loc, bool fahrenheit, const transit::HttpGet &http) {
  std::string url = weather::openMeteoUrl(loc.lat, loc.lng, fahrenheit, kForecastHours);
  std::vector<uint8_t> body;
  int status = http(url, [&](const uint8_t *d, size_t n) {
    if (body.size() + n > kBodyCap) return false;
    body.insert(body.end(), d, d + n);
    return true;
  });
  weather::Forecast f;
  std::string err;
  bool ok = !body.empty() && weather::parseOpenMeteo(body.data(), body.size(), &f, &err);
  Serial.printf("[weather] %.4f,%.4f: HTTP %d, %u bytes%s%s\n", loc.lat, loc.lng, status, (unsigned)body.size(),
                ok ? ", " : ", parse failed: ", ok ? weather::codeText(f.code) : err.c_str());
  if (ok) {
    loc.forecast = std::move(f);
    loc.ok = true;
  }
  loc.fetched_ms = millis();  // even on failure: don't hammer the API every 30 s
  return ok;
}

std::string clockText(int64_t epoch) {
  time_t t = (time_t)epoch;
  struct tm lt;
  localtime_r(&t, &lt);
  int h12 = lt.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d%s", h12, lt.tm_min, lt.tm_hour < 12 ? "a" : "p");
  return buf;
}

}  // namespace

void invalidateWeather() {
  g_invalidate = true;
}

void refreshWeather(const Config &cfg, const transit::HttpGet &http) {
  if (!cfg.weather.enabled) {
    Lock lock;
    if (!lock.held) return;
    g_enabled = false;
    g_locations.clear();
    g_stops.clear();
    return;
  }

  // Work on a private copy; publish under the mutex once the (slow) fetches are done.
  std::vector<Location> locs;
  std::vector<StopLocation> stops;
  bool regrouped = false;
  {
    Lock lock;
    if (!lock.held) return;
    std::string fp = stopsFingerprint(cfg);
    if (g_invalidate || g_fahrenheit != cfg.weather.fahrenheit || g_per_stop != cfg.weather.per_stop || fp != g_fingerprint) {
      g_invalidate = false;
      g_fingerprint = fp;
      regroup(cfg, locs, stops);
      regrouped = true;
    } else {
      locs = g_locations;
      stops = g_stops;
    }
  }

  bool any_fetched = false;
  for (Location &loc : locs) {
    bool due = regrouped || !loc.ok || (millis() - loc.fetched_ms) >= kRefreshMs;
    if (!due) continue;
    if (fetchLocation(loc, cfg.weather.fahrenheit, http)) any_fetched = true;
  }

  Lock lock;
  if (!lock.held) return;
  g_enabled = true;
  g_fahrenheit = cfg.weather.fahrenheit;
  g_per_stop = cfg.weather.per_stop;
  g_locations = std::move(locs);
  g_stops = std::move(stops);
  if (any_fetched && !g_locations.empty() && g_locations[0].ok) g_fetched_epoch = (uint32_t)time(nullptr);
}

WeatherView getWeather() {
  WeatherView v;
  Lock lock;
  if (!lock.held) return v;
  v.enabled = g_enabled;
  v.fahrenheit = g_fahrenheit;
  v.fetched_epoch = g_fetched_epoch;
  if (!g_locations.empty() && g_locations[0].ok) v.main = g_locations[0].forecast;
  return v;
}

std::string headerWeatherText() {
  Lock lock;
  if (!lock.held || !g_enabled || g_locations.empty() || !g_locations[0].ok) return "";
  const weather::Forecast &f = g_locations[0].forecast;
  char buf[48];
  snprintf(buf, sizeof(buf), "%d\xC2\xB0 %s", (int)lround(f.temp), weather::codeText(f.code));
  return buf;
}

std::string stopWeatherNote(const std::string &stop_key, int64_t first_arrival_epoch) {
  Lock lock;
  if (!lock.held || !g_enabled || !g_per_stop || first_arrival_epoch <= 0) return "";
  if (g_locations.empty() || !g_locations[0].ok) return "";
  int idx = 0;
  for (const StopLocation &s : g_stops) {
    if (s.key == stop_key) {
      idx = s.location >= 0 ? s.location : 0;
      break;
    }
  }
  if ((size_t)idx >= g_locations.size() || !g_locations[(size_t)idx].ok) idx = 0;
  const weather::Hour *h = g_locations[(size_t)idx].forecast.at(first_arrival_epoch);
  if (h == nullptr) return "";
  if (!weather::notable(*h, g_locations[0].forecast.code)) return "";

  char buf[64];
  if (weather::isWet(weather::kindForCode(h->code))) {
    snprintf(buf, sizeof(buf), "%s at %s, %d\xC2\xB0", weather::codeText(h->code), clockText(first_arrival_epoch).c_str(), (int)lround(h->temp));
  } else if (h->precip_prob >= 40) {
    snprintf(buf, sizeof(buf), "rain likely (%d%%) at %s", h->precip_prob, clockText(first_arrival_epoch).c_str());
  } else {
    snprintf(buf, sizeof(buf), "%s at %s, %d\xC2\xB0", weather::codeText(h->code), clockText(first_arrival_epoch).c_str(), (int)lround(h->temp));
  }
  return buf;
}

}  // namespace transit_app
