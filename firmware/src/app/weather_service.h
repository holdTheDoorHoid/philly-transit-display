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

struct WeatherView {
  bool enabled = false;
  bool fahrenheit = true;
  uint32_t fetched_epoch = 0;  // wall clock of the last successful main fetch, 0 = none yet
  weather::Forecast main;
};
WeatherView getWeather();  // thread-safe copy

// "69° mostly clear" for the header; empty when disabled or nothing fetched yet.
std::string headerWeatherText();
// Header pieces for the icon form: which of the colour icons in src/icons/ fits the current WMO
// code (None when there is no forecast) and the temperature alone ("69°"). Night (before 6 or
// from 20 local) swaps sun for moon.
enum class WeatherIcon : uint8_t { None, Sun, Moon, CloudSun, CloudMoon, Cloud, Rain, Showers, Snow, Fog, Storm };
WeatherIcon headerWeatherIcon();
std::string headerWeatherTemp();

// One line for a stop panel about the forecast at `first_arrival_epoch` ("light rain at 10:15a,
// 62°", "rain likely (55%) at 10:15a"), or empty when it would just repeat the header
// (weather::notable against the main location's current conditions), when weather.per_stop is
// off, or when nothing is known. `now` picks the clock wording.
std::string stopWeatherNote(const std::string &stop_key, int64_t first_arrival_epoch);

}  // namespace transit_app
