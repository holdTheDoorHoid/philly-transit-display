// Time-to-leave alert (DESIGN.md SS6 "due"): when an arrival first comes within due.minutes the
// RGB LED blinks green, the row's minutes blink (main_screen.cpp asks arrivalIsDue()), and the
// speaker plays two short beeps once per trip. Driven once per second from the LVGL task.
#pragma once
#include <string>
#include <vector>

#include "config_store.h"
#include "transit_core/model.h"

namespace transit_app {

// True for a live/scheduled arrival between -1 min and due.minutes from now.
bool arrivalIsDue(const Config &cfg, const transit::Arrival &a, transit::Epoch now);

// Call once per second with the stops currently shown. `quiet` (quiet hours) silences the chime.
// Returns true while at least one shown arrival is due, so the caller can blink the screen.
bool dueAlertTick(const Config &cfg, const transit::Snapshot &snap, const std::vector<std::string> &shown_keys,
                  bool quiet, transit::Epoch now);

}  // namespace transit_app
