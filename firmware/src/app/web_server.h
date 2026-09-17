// ESPAsyncWebServer routes, DESIGN.md SS7.
//
// Two classes of route (DESIGN.md SS12, review F01): *viewing* is open - GET /api/state,
// GET /api/config, /api/stats*, /api/log/index, /api/proxy/*, /api/rail/*, GET /api/debug/ui and
// the static assets answer anyone on the LAN, so the display keeps behaving like an appliance.
// Everything that *changes* the device - PUT /api/config, POST /api/reboot, /api/wifi/reset,
// /api/ota, /api/pin, /api/debug/tap - plus the log CSV download (a month of the owner's
// movements) requires the admin PIN in an `X-Pin` header; see auth.h for where the PIN lives and
// how the owner recovers it. Missing/wrong PIN: 401. Five wrong in a row: 429 for 30 s.
//
// Every request also has its `Host` header checked against the ways this device can legitimately
// be addressed; anything else gets 421 (DNS-rebinding defence, review F05).
#pragma once
#include <cstdint>
#include <functional>

namespace transit_app {

// Starts the web server on port 80. Reads/writes the live config via
// config_store::getActiveConfig()/setActiveConfig(); `onConfigChanged`, if
// given, runs after a PUT /api/config validates and persists successfully
// (DESIGN.md SS7: "triggers immediate re-poll"), with `true` when the parts of the config that
// decide what is fetched changed. It runs on the async web server's own task - main.cpp hands
// anything that must not run there (mDNS, NTP) to loop().
//
// auth::begin() must have run first: the PIN guard on the state-changing routes reads from it.
void startWebServer(std::function<void(bool)> onConfigChanged = nullptr);

// True while a POST /api/ota upload is streaming into the flash partition. Read from loopTask by
// main.cpp's poller-liveness check (DESIGN.md SS12.1): an OTA legitimately starves the poller for
// the length of the upload, and rebooting mid-write would leave a half-written partition, so the
// liveness net stands down while this is true. Not synchronised - it is a bool written once at the
// start of an upload and once at its end, and the reader only cares about a state that lasts for
// the whole upload.
bool otaBusy();

// How many AsyncWebServerRequest objects are alive right now (admission.h). Read by the poller's
// queued-job gate: a ~9 KB stats scan must not start while a burst of web requests is in flight,
// which is what crashed the 0.3.1-rc2 device suite. A few milliseconds stale by construction,
// which is fine for a question that only has to distinguish "quiet" from "burst".
uint32_t inFlightRequests();

// Connections closed at accept because the in-flight cap or a heap floor refused them, since boot.
uint32_t admissionRefusals();

}  // namespace transit_app
