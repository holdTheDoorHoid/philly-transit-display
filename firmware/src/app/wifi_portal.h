// Minimal Wi-Fi onboarding: try the ESP32's own NVS-persisted STA credentials first, then fall
// back to a small SoftAP + captive portal of our own. Replaces tzapu/WiFiManager (DESIGN.md SS2
// picked it for turnkey familiarity, but its whole-library cost is not worth ~150 lines of glue
// on a device this flash-starved - see firmware/README.md's "Memory and flash budget" section).
//
// Bonus: WiFiManager was also the source of the `task_wdt: esp_task_wdt_reset(705): task not
// found` log spam on Arduino-ESP32 core 3.x (it touches the task watchdog from a task it doesn't
// own); removing it removes that noise too.
#pragma once
#include <functional>
#include <string>

namespace transit_app {

// Tries WiFi.begin() (no args - reconnects with whatever the ESP-IDF Wi-Fi driver already has
// persisted in NVS from a previous successful connection) for up to ~20 s. If that connects,
// returns immediately (Wi-Fi is up, caller continues setup()).
//
// Otherwise starts a "<ap_name>" SoftAP at 192.168.4.1 with a DNS server answering every name
// with that address (captive-portal detection) and serves a tiny inline page (PROGMEM, no
// external assets) listing scanned SSIDs with a password field. Saving there calls
// WiFi.begin(ssid, pass) (persists by default), and the device reboots on a successful
// connection. This function does not return in that path - either Wi-Fi comes up here, or the
// device reboots and main.cpp's setup() runs again from the top.
//
// `pump` is invoked frequently (at least every ~20 ms) throughout, so the caller's LVGL loop
// keeps rendering during both the stored-credentials wait and the portal's lifetime.
void connectWifiOrPortal(const std::string &ap_name, const std::function<void()> &pump);

}  // namespace transit_app
