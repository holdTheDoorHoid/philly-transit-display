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

// Failure backoff (F29). Before this, `due` was `!loc.ok || age >= kRefreshMs`, and `!loc.ok`
// stays true until the FIRST success - so a location that had never answered was refetched on
// every poll cycle, i.e. every 30 s (every 15 s with a bus due), forever. That is the exact
// opposite of what a failing endpoint deserves, it is rude to a free no-key API, and it spent the
// poller's budget on the one request least likely to work. 1 min, then 5, then 10 and stay there.
uint32_t retryDelayMs(uint8_t consecutive_failures) {
  if (consecutive_failures <= 1) return 1u * 60 * 1000;
  if (consecutive_failures == 2) return 5u * 60 * 1000;
  return 10u * 60 * 1000;
}

struct Location {
  double lat = 0;
  double lng = 0;
  weather::Forecast forecast;
  // These three used to be one field, which is why a failed fetch could look like a fresh one:
  // `fetched_ms` was stamped on every attempt so the data could age without the age moving.
  uint32_t attempted_ms = 0;    // last attempt, success or not - drives the backoff
  uint32_t fetched_ms = 0;      // last SUCCESS - drives freshness and staleness
  uint32_t fetched_epoch = 0;   // wall clock of that success, for the API's fetched_epoch
  uint8_t consecutive_failures = 0;
  bool ok = false;              // a forecast is present (possibly old - check fetched_ms)
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
  loc.attempted_ms = millis();  // stamped on every attempt: this is what the backoff measures
  if (ok) {
    loc.forecast = std::move(f);
    loc.ok = true;
    loc.fetched_ms = loc.attempted_ms;  // only a SUCCESS makes the data young again
    loc.fetched_epoch = (uint32_t)time(nullptr);
    loc.consecutive_failures = 0;
  } else if (loc.consecutive_failures < 250) {
    loc.consecutive_failures++;
  }
  return ok;
}

// Age of a location's data in seconds, or -1 if it has never succeeded. Measured from millis()
// rather than the wall clock so an NTP step (the device polls before NTP lands - net_poller.cpp)
// cannot invent an hour of age or erase one. Unsigned subtraction handles the 49-day wrap.
int32_t locationAgeS(const Location &loc) {
  if (!loc.ok || loc.fetched_ms == 0) return -1;
  return (int32_t)((millis() - loc.fetched_ms) / 1000u);
}

// Callers must hold the lock. "Fresh" = we have it AND it is younger than kWeatherStaleAfterS.
bool locationFresh(const Location &loc) {
  int32_t age = locationAgeS(loc);
  return age >= 0 && (uint32_t)age <= kWeatherStaleAfterS;
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

  for (Location &loc : locs) {
    bool due;
    if (regrouped || loc.attempted_ms == 0) {
      due = true;  // a config change, or a location never tried
    } else if (loc.consecutive_failures > 0) {
      due = (millis() - loc.attempted_ms) >= retryDelayMs(loc.consecutive_failures);
    } else {
      due = (millis() - loc.fetched_ms) >= kRefreshMs;  // DESIGN.md SS4.8: 10 min per location
    }
    if (!due) continue;
    fetchLocation(loc, cfg.weather.fahrenheit, http);
  }

  Lock lock;
  if (!lock.held) return;
  g_enabled = true;
  g_fahrenheit = cfg.weather.fahrenheit;
  g_per_stop = cfg.weather.per_stop;
  g_locations = std::move(locs);
  g_stops = std::move(stops);
  // MAIN location only (F29). This used to be bumped whenever ANY location fetched successfully
  // while location 0 merely had `ok` still true from some earlier poll - so a second stop's
  // forecast refreshing made the header's hours-old temperature report as seconds old, which is
  // the one thing the timestamp exists to prevent.
  g_fetched_epoch = g_locations.empty() ? 0 : g_locations[0].fetched_epoch;
}

WeatherView getWeather() {
  WeatherView v;
  Lock lock;
  if (!lock.held) return v;
  v.enabled = g_enabled;
  v.fahrenheit = g_fahrenheit;
  v.fetched_epoch = g_fetched_epoch;
  if (!g_locations.empty()) {
    v.age_s = locationAgeS(g_locations[0]);
    v.stale = v.age_s >= 0 && (uint32_t)v.age_s > kWeatherStaleAfterS;
    // The forecast is handed over even when stale so the web UI can show it next to its own age
    // and decide; only the DEVICE's header and notes go silent, because there is no room on a
    // 480x320 panel to caption a number with how old it is (DESIGN.md SS8).
    if (g_locations[0].ok) v.main = g_locations[0].forecast;
  }
  return v;
}

std::string headerWeatherText() {
  Lock lock;
  if (!lock.held || !g_enabled || g_locations.empty() || !locationFresh(g_locations[0])) return "";
  const weather::Forecast &f = g_locations[0].forecast;
  char buf[48];
  snprintf(buf, sizeof(buf), "%d\xC2\xB0 %s", (int)lround(f.temp), weather::codeText(f.code));
  return buf;
}

static WeatherIcon iconForCode(int code, bool night) {
  if (code < 0) return WeatherIcon::None;
  if (code <= 1) return night ? WeatherIcon::Moon : WeatherIcon::Sun;
  if (code == 2) return night ? WeatherIcon::CloudMoon : WeatherIcon::CloudSun;
  if (code == 3) return WeatherIcon::Cloud;
  if (code == 45 || code == 48) return WeatherIcon::Fog;
  if (code >= 51 && code <= 67) return WeatherIcon::Rain;  // drizzle, rain, freezing rain
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WeatherIcon::Snow;
  if (code >= 80 && code <= 82) return WeatherIcon::Showers;
  if (code >= 95) return WeatherIcon::Storm;
  return WeatherIcon::Cloud;
}

WeatherIcon headerWeatherIcon() {
  Lock lock;
  if (!lock.held || !g_enabled || g_locations.empty() || !locationFresh(g_locations[0])) return WeatherIcon::None;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  bool night = lt.tm_hour < 6 || lt.tm_hour >= 20;
  return iconForCode(g_locations[0].forecast.code, night);
}

std::string headerWeatherTemp() {
  Lock lock;
  if (!lock.held || !g_enabled || g_locations.empty() || !locationFresh(g_locations[0])) return "";
  char buf[16];
  snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lround(g_locations[0].forecast.temp));
  return buf;
}

std::string stopWeatherNote(const std::string &stop_key, int64_t first_arrival_epoch) {
  Lock lock;
  if (!lock.held || !g_enabled || !g_per_stop || first_arrival_epoch <= 0) return "";
  // The note is judged AGAINST the header's conditions, so a stale main forecast makes the whole
  // comparison meaningless, not just the header (F29).
  if (g_locations.empty() || !locationFresh(g_locations[0])) return "";
  int idx = 0;
  for (const StopLocation &s : g_stops) {
    if (s.key == stop_key) {
      idx = s.location >= 0 ? s.location : 0;
      break;
    }
  }
  if ((size_t)idx >= g_locations.size() || !locationFresh(g_locations[(size_t)idx])) idx = 0;
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
