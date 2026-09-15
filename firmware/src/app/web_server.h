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

}  // namespace transit_app
