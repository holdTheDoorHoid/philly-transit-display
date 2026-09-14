// Top-level UI: owns the three screens and the tap-to-cycle behavior.
// DESIGN.md SS8: "Tap anywhere cycles Main -> Stats -> Device info -> Main."
#pragma once
#include <string>

#include "../config_store.h"

namespace transit_app::ui {

// Builds all three screens and shows Main. Call once, after
// smartdisplay_init()/lv_display_set_rotation() have run.
void init(const Config &cfg);

// Full-screen "connect to <ap_name> to set up Wi-Fi" message, shown while
// WiFiManager's captive portal is open. May be called before init() (it
// creates and loads its own throwaway screen) - main.cpp wires this to
// WiFiManager::setAPCallback(), which only fires if there's no saved
// network to reconnect to.
void showWifiSetupScreen(const std::string &ap_name);

// Refreshes the currently visible screen (clock, Wi-Fi bars, "updated Ns
// ago", the demo Snapshot's arrivals, device info fields). Call this
// periodically (main.cpp does so from an LVGL timer at ~1 Hz). No-op until
// init() has run.
void tick();

}  // namespace transit_app::ui
