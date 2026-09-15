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

// Brings Wi-Fi up, or hands the device to the setup portal, and does not return in the second
// case. Two paths (DESIGN.md SS12, review F02/F10):
//
// PROVISIONED (the ESP-IDF Wi-Fi driver has a stored SSID): tries it for ~20 s, and if that
// fails keeps retrying it forever with a 5 s -> 60 s backoff while the panel shows
// "Connecting to <ssid>..." and "Tap the screen to open Wi-Fi setup instead". It deliberately
// does NOT open the setup AP by itself: every router reboot would otherwise put an AP named
// after this device on the air with a /save endpoint that can overwrite the owner's stored
// network. Only a touch on the panel opens the portal.
//
// UNPROVISIONED (or the owner tapped): starts a "<ap_name>" SoftAP at 192.168.4.1 secured with
// WPA2 - auth::apPassword(), shown on the panel with a QR code - plus a DNS server answering
// every name with that address (captive-portal detection) and a tiny inline page (PROGMEM, no
// external assets) listing scanned SSIDs. Saving there attempts the connection from this
// function's own pump loop (one attempt at a time; a second POST /save gets 409) and reboots on
// success. The portal is bounded: ten minutes with no client joined to the AP reboots back into
// the retry loop above, so the device is not left open indefinitely.
//
// `pump` is invoked frequently (at least every ~20 ms) throughout, so the caller's LVGL loop
// keeps rendering - and keeps processing touches, which is how the tap-to-setup gesture works.
//
// auth::begin() must have run first (the AP password comes from it).
void connectWifiOrPortal(const std::string &ap_name, const std::function<void()> &pump);

}  // namespace transit_app
