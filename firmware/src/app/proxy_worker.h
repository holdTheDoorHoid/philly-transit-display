// Worker task for the setup-wizard proxy endpoints (DESIGN.md SS7):
//   GET /api/proxy/stops?route=<route>       -> SEPTA Stops/index.php, verbatim
//   GET /api/proxy/schedule?stop_id=<id>     -> SEPTA BusSchedules/index.php, verbatim
// Async web handlers must never block on a network round trip (that would stall every other
// request and starve LVGL's own network task, DESIGN.md SS5), so these two routes only queue a
// job here and return immediately; this file's own task does the actual SEPTA fetch and calls
// request->send() itself once it has a result.
#pragma once
#include <ESPAsyncWebServer.h>

#include <string>

namespace transit_app {

// Starts the worker task. Call once from startWebServer().
void startProxyWorker();

// Queues a Stops proxy job for `route` and takes ownership of `request` (the caller must not
// touch it again). Responds 200 with SEPTA's JSON verbatim, or 502 {"error"} on any failure
// (bad status, oversized body, or transport error); guards against the client having
// disconnected before the fetch completes.
void queueStopsProxy(AsyncWebServerRequest *request, const std::string &route);

// Same contract as queueStopsProxy, for BusSchedules by `stop_id`.
void queueScheduleProxy(AsyncWebServerRequest *request, const std::string &stop_id);

}  // namespace transit_app
