// Weather for the header and the stop panels (DESIGN.md SS4.8, SS8): fetches Open-Meteo
// forecasts for the configured stops' locations on the poller task and serves them to the UI and
// the web server. weather_core (lib/) owns the parsing and the vocabulary; this file owns the
// scheduling, the grouping of nearby stops into one request, and the wording.
#pragma once
#include <cstdint>
#include <string>

#include "config_store.h"
#include "transit_core/source.h"
#include "weather_core/weather.h"

namespace transit_app {

// Called once per poll cycle. Fetches at most every 10 minutes per location; stops within
// 1.5 km share one request; at most 4 locations. The first configured stop with coordinates is
// the "main" location the header shows. Stops without coordinates use the main forecast.
void refreshWeather(const Config &cfg, const transit::HttpGet &http);

// Config changed: regroup and refetch on the next poll.
void invalidateWeather();

// How old the MAIN location's forecast may be before the device stops showing it (F29). An
// Open-Meteo "current conditions" block an hour old is not weather, it is a memory: the header
// would keep reading 69° and sunny through an evening thunderstorm with nothing on screen to say
// the number had stopped moving. Past this age the header pieces and the stop notes go empty and
// the UI falls back to showing nothing, which is the honest answer.
constexpr uint32_t kWeatherStaleAfterS = 60 * 60;

struct WeatherView {
  bool enabled = false;
  bool fahrenheit = true;
  uint32_t fetched_epoch = 0;  // wall clock of the last successful MAIN fetch, 0 = none yet
  // Seconds since that main fetch, or -1 when there has never been one. Measured from millis(),
  // not from the clock, so an NTP step does not invent or erase an age (F29). This is the value
  // GET /api/state's weather.age_s should report: it answers "how old is what you are looking
  // at", which `fetched_epoch` alone could not once a non-main location's success was allowed to
  // bump it.
  int32_t age_s = -1;
  bool stale = false;  // age_s > kWeatherStaleAfterS; the header/notes are suppressed
  weather::Forecast main;
};
WeatherView getWeather();  // thread-safe copy

// "69° mostly clear" for the header; empty when disabled, when nothing has been fetched yet, or
// when the main forecast has gone stale (kWeatherStaleAfterS).
std::string headerWeatherText();
// Header pieces for the icon form: which of the colour icons in src/icons/ fits the current WMO
// code (None when there is no forecast, or it is stale) and the temperature alone ("69°"). Night
// (before 6 or from 20 local) swaps sun for moon.
enum class WeatherIcon : uint8_t { None, Sun, Moon, CloudSun, CloudMoon, Cloud, Rain, Showers, Snow, Fog, Storm };
WeatherIcon headerWeatherIcon();
std::string headerWeatherTemp();

// One line for a stop panel about the forecast at `first_arrival_epoch` ("light rain at 10:15a,
// 62°", "rain likely (55%) at 10:15a"), or empty when it would just repeat the header
// (weather::notable against the main location's current conditions), when weather.per_stop is
// off, when the forecast is stale, or when nothing is known. `now` picks the clock wording.
std::string stopWeatherNote(const std::string &stop_key, int64_t first_arrival_epoch);

}  // namespace transit_app
