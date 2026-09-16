// Top-level UI: owns the three screens and the tap-to-cycle behavior.
// DESIGN.md SS8: "Tap anywhere cycles Main -> Stats -> Device info -> Main."
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../config_store.h"

namespace transit_app::ui {

// Builds the arrivals and night screens and shows Main. The stats and device pages are built
// when tapped to and freed when tapped away from (ui.cpp buildScreens() says why: LVGL's pool).
// Call once, after smartdisplay_init()/lv_display_set_rotation() have run.
void init(const Config &cfg);

// Full-screen "join this network, then open http://192.168.4.1" message, shown while the setup
// portal is open (wifi_portal.cpp). Carries the AP's name, its WPA2 password in the big font, and
// a QR code of the standard `WIFI:` URI so a phone can join by pointing its camera at the panel
// instead of typing ten characters (DESIGN.md SS12, review F02/F10). May be called before init():
// it creates and loads its own screen.
void showWifiSetupScreen(const std::string &ap_name, const std::string &password);

// Full-screen "Connecting to <ssid>..." message with the line "Tap the screen to open Wi-Fi setup
// instead", shown while wifi_portal.cpp retries a *provisioned* device's stored network (review
// F10: a router reboot must not put the owner's home credentials back on the air in an open
// setup AP). Also creates and loads its own screen, and attaches the tap handler consumeTap()
// reads. Safe to call repeatedly - later calls only update the text.
void showConnectingScreen(const std::string &ssid, const std::string &detail);

// Reads and clears the "the screen was tapped" flag. For callers that run before init(), where
// tick() is not pumping yet - wifi_portal.cpp's retry loop uses it to decide whether the owner
// asked for the setup portal. Both a real touch on the connecting screen and POST /api/debug/tap
// set the flag.
bool consumeTap();

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

// Turns the panel controller's colour inversion on or off (config.device.invert_colors). IPS
// variants of the Sunton boards need it on to show colours as drawn; without it a near-black
// background renders as white (docs/hardware.md). Same task rules as applyRotation().
void applyInvert(bool invert);

// Selects the light or dark palette (config.device.theme); see ui_common.h setTheme(). Before
// init() only; afterwards onConfigChanged() applies it with the screen rebuild.
void setTheme(const std::string &name);

// Hands a new configuration to the UI from any task. The next tick() (LVGL task) applies
// rotation and brightness and rebuilds the screens so new stops appear without a reboot.
void onConfigChanged(const Config &cfg);

// Prints LVGL pool usage over serial (sizing LV_MEM_SIZE).
void logMemory();

// Test hooks for GET /api/debug/ui and POST /api/debug/tap (DESIGN.md SS7): what the screen is
// doing right now, and a simulated touch (press + click on the LVGL task, exactly the path a
// finger takes, so quiet-hours wake and page cycling can be exercised without the panel).
struct UiDebug {
  std::string page;            // "main", "night", "stats", "device"
  bool dimmed = false;
  int brightness = -1;         // percent actually applied
  bool due_active = false;
  uint32_t chimes = 0;
  std::string active_profile;
  std::vector<std::string> shown_stops;
  std::vector<std::string> hidden_panels;  // alternatives currently hidden
  std::string ticker;
  std::string rows;            // main screen rows (main_screen.h mainScreenDebug)
  std::string header_weather;
  uint32_t lv_used = 0, lv_free = 0, lv_max_used = 0;
  int32_t hor_res = 0, ver_res = 0;
};
UiDebug debugSnapshot();
void requestTap();

}  // namespace transit_app::ui
