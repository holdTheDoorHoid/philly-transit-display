// ESPAsyncWebServer routes, DESIGN.md SS7. Implemented for real: GET
// /api/state, GET/PUT /api/config, POST /api/reboot, POST /api/wifi/reset,
// and a placeholder GET /. Every other SS7 route responds 501 JSON.
#pragma once
#include <functional>

namespace transit_app {

// Starts the web server on port 80. Reads/writes the live config via
// config_store::getActiveConfig()/setActiveConfig(); `onConfigChanged`, if
// given, runs after a PUT /api/config validates and persists successfully
// (DESIGN.md SS7: "triggers immediate re-poll" - wiring that trigger
// through to net_poller is left to main.cpp once net_poller supports
// changing its interval without a reboot).
void startWebServer(std::function<void(bool)> onConfigChanged = nullptr);

}  // namespace transit_app
