// Knobs for the fake app services the simulator links instead of the real ones (fakes.cpp).
// The screens under src/app/ui/ call auth::pin(), getSdStatus(), getStopSummary(), getBikes(),
// the weather header helpers and the due-alert tick; on the host every one of those answers from
// the values set here, so a render is deterministic and needs no Wi-Fi, SD card or SEPTA.
#pragma once
#include <string>

#include "app/net_poller.h"

namespace sim {

// Replaces the canned stats summary for one stop key (getStopSummary() returns a default
// "loading" view for keys never set).
void setSummary(const std::string &key, const transit_app::StopSummaryView &v);
void clearSummaries();

// A fully populated summary: `pct` on-time percent (has_on_time false when pct < 0), mean lateness,
// worst hour (-1 = unknown), ghosts, sample count and how many were inferred.
transit_app::StopSummaryView summary(float pct, float mean_late, int worst_hour, unsigned ghosts, unsigned samples,
                                     unsigned inferred);

extern bool bikes_enabled;
extern bool sd_mounted;
extern uint32_t sd_dropped_rows;   // SdStatus write health on the device page (F26)
extern std::string sd_error;       // "" = last write ok
extern bool poll_ok;               // the device page's "SEPTA ok / failed" line
extern uint32_t poll_age_s;
extern std::string poll_error;
extern std::string weather_temp;   // "" hides the header weather
extern std::string weather_text;
extern int weather_icon;           // transit_app::WeatherIcon as an int
extern std::string stop_note;      // per-stop weather note shown on every panel, "" for none

}  // namespace sim
