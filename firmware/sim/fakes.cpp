// Fake app services for the screenshot simulator (firmware/sim/README.md). These replace the
// firmware's auth/sd_logger/net_poller/bike_service/weather_service/due_alert *implementations*
// while the screens keep including the real headers, so a screen that starts calling something
// new fails to link here rather than silently rendering with a stub nobody updated.
#include <ctime>
#include <map>
#include <string>

#include "app/auth.h"
#include "app/bike_service.h"
#include "app/due_alert.h"
#include "app/net_poller.h"
#include "app/sd_logger.h"
#include "app/weather_service.h"
#include "sim_fakes.h"
#include "stubs/Arduino.h"
#include "stubs/WiFi.h"

// ---- Arduino / WiFi objects the stub headers declare ----
namespace sim {
uint32_t millis_ms = (2 * 86400 + 5 * 3600 + 14 * 60) * 1000u;  // the device page's uptime line: "up 2d 5h"
uint32_t free_heap = 75 * 1024 + 300;
bool wifi_connected = true;
int wifi_rssi = -61;
std::string wifi_ssid = "Fios-X42QE";
std::string wifi_ip = "192.168.1.181";
bool bikes_enabled = true;
bool sd_mounted = true;
uint32_t sd_dropped_rows = 0;
std::string sd_error;
bool poll_ok = true;
uint32_t poll_age_s = 12;
std::string poll_error;
int32_t weather_age_s = 4 * 60;
int32_t bike_age_s = 90;
int32_t alerts_age_s = 3 * 60;
std::string weather_temp = "63\xC2\xB0";
std::string weather_text = "63\xC2\xB0 clear";
int weather_icon = 1;  // Sun
std::string stop_note;

namespace {
std::map<std::string, transit_app::StopSummaryView> g_summaries;
}  // namespace

void setSummary(const std::string &key, const transit_app::StopSummaryView &v) {
  g_summaries[key] = v;
}
void clearSummaries() {
  g_summaries.clear();
}
transit_app::StopSummaryView summary(float pct, float mean_late, int worst_hour, unsigned ghosts, unsigned samples,
                                     unsigned inferred) {
  transit_app::StopSummaryView v;
  v.has_value = true;
  v.inferred = inferred;
  v.summary.samples = samples;
  v.summary.late_known = pct >= 0 ? samples : 0;
  v.summary.has_on_time = pct >= 0;
  v.summary.on_time_pct = pct >= 0 ? pct : 0.0f;
  v.summary.mean_late_min = mean_late;
  v.summary.worst_hour = (int8_t)worst_hour;
  v.summary.ghosts = ghosts;
  return v;
}
}  // namespace sim

SimEsp ESP;
SimWiFi WiFi;

namespace transit_app {

// ---- auth.h ----
namespace auth {
const std::string &pin() {
  static const std::string kPin = "123456";  // the README's example PIN, never a real device's
  return kPin;
}
}  // namespace auth

// ---- sd_logger.h ----
SdStatus getSdStatus() {
  SdStatus s;
  s.mounted = sim::sd_mounted;
  s.total_bytes = 3900ull * 1024 * 1024;
  s.free_bytes = 3720ull * 1024 * 1024;
  s.used_bytes = s.total_bytes - s.free_bytes;
  s.dropped_rows = sim::sd_dropped_rows;
  s.last_write_ok = sim::sd_error.empty();
  s.error = sim::sd_error;
  return s;
}

// ---- net_poller.h ----
PollStatus getPollStatus() {
  PollStatus p;
  p.has_polled = true;
  p.ok = sim::poll_ok;
  p.last_poll_epoch = (uint32_t)time(nullptr) - sim::poll_age_s;
  p.last_error = sim::poll_error;
  return p;
}
AlertsStatus getAlertsStatus() {
  AlertsStatus s;
  if (sim::alerts_age_s >= 0) {
    s.fetched = true;
    s.age_s = (uint32_t)sim::alerts_age_s;
  }
  return s;
}

StopSummaryView getStopSummary(const std::string &stop_key) {
  auto it = sim::g_summaries.find(stop_key);
  if (it == sim::g_summaries.end()) return StopSummaryView();  // "loading": nothing cached yet
  return it->second;
}

// ---- bike_service.h ----
BikeView getBikes() {
  BikeView v;
  v.enabled = sim::bikes_enabled;
  if (!v.enabled) return v;
  v.fetched_epoch = sim::bike_age_s < 0 ? 0 : (uint32_t)time(nullptr) - (uint32_t)sim::bike_age_s;
  indego::Station a;
  a.id = 3005;
  a.name = "Snyder & Dorrance";
  a.active = true;
  a.bikes = 7;
  a.classic = 5;
  a.ebikes = 2;
  a.docks = 8;
  indego::Station b;
  b.id = 3121;
  b.name = "18th & Fernon, Aquinas Center";
  b.active = true;
  b.bikes = 1;
  b.classic = 1;
  b.ebikes = 0;
  b.docks = 14;
  v.stations = {a, b};
  return v;
}

// ---- weather_service.h ----
WeatherView getWeather() {
  WeatherView v;
  v.enabled = true;
  v.age_s = sim::weather_age_s;
  v.stale = v.age_s > (int32_t)kWeatherStaleAfterS;
  v.fetched_epoch = v.age_s < 0 ? 0 : (uint32_t)time(nullptr) - (uint32_t)v.age_s;
  return v;
}
std::string headerWeatherText() {
  return sim::weather_text;
}
WeatherIcon headerWeatherIcon() {
  return sim::weather_temp.empty() ? WeatherIcon::None : (WeatherIcon)sim::weather_icon;
}
std::string headerWeatherTemp() {
  return sim::weather_temp;
}
std::string stopWeatherNote(const std::string &, int64_t) {
  return sim::stop_note;
}

// ---- due_alert.h ----
bool arrivalIsDue(const Config &, const transit::Arrival &, transit::Epoch) {
  return false;
}
uint32_t dueChimesPlayed() {
  return 0;
}
bool dueAlertTick(const Config &, const transit::Snapshot &, const std::vector<std::string> &, bool, transit::Epoch) {
  return false;
}

}  // namespace transit_app
