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

// Applies `percent` (0-100, config.device.brightness) to the panel backlight via
// esp32_smartdisplay's smartdisplay_lcd_set_backlight(). Call once at boot (after init()) and
// again whenever PUT /api/config changes device.brightness (DESIGN.md task 8).
void applyBrightness(uint8_t percent);

// Rotates the display to `degrees` (0/90/180/270, config.device.rotation). Must be called from
// the LVGL task; before init() it only rotates, after init() call onConfigChanged() instead.
void applyRotation(uint16_t degrees);

// Hands a new configuration to the UI from any task. The next tick() (LVGL task) applies
// rotation and brightness and rebuilds the screens so new stops appear without a reboot.
void onConfigChanged(const Config &cfg);

}  // namespace transit_app::ui
