#include "web_server.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>

#include "config_store.h"
#include "demo_data.h"
#include "net_poller.h"
#include "sd_logger.h"

// The `web` agent's build.mjs produces this from web/ (DESIGN.md SS10); it
// doesn't exist yet, so this include is guarded to keep this firmware
// buildable either way. This skeleton doesn't yet consume anything from it
// (no agreed API to serve gzipped assets from it exists yet) - "/" always
// serves the placeholder page below for now; wiring it up for real is
// follow-up work once web_assets.h's actual contents/API are defined.
#if __has_include("generated/web_assets.h")
#include "generated/web_assets.h"
#endif

using transit::Alert;
using transit::Arrival;
using transit::Snapshot;
using transit::Status;
using transit::StopSnapshot;

namespace transit_app {

namespace {

AsyncWebServer g_server(80);

const char *statusToString(Status s) {
  switch (s) {
    case Status::Live:
      return "live";
    case Status::Scheduled:
      return "scheduled";
    case Status::Skipped:
      return "skipped";
    case Status::Unknown:
    default:
      return "unknown";
  }
}

// DESIGN.md SS7's Arrival object shape.
void serializeArrival(const Arrival &a, JsonObject o, transit::Epoch now) {
  o["trip"] = a.trip;
  o["vehicle"] = a.vehicle;
  o["destination"] = a.destination;
  o["predicted"] = a.predicted;
  o["scheduled"] = a.scheduled;
  o["eta_s"] = a.effective() > 0 ? (a.effective() - now) : 0;
  o["late_min"] = a.late_min;
  o["late_known"] = a.late_known;
  o["status"] = statusToString(a.status);
  o["seats"] = a.seats;
}

void serializeSnapshot(const Snapshot &snap, JsonObject out) {
  transit::Epoch now = (transit::Epoch)time(nullptr);
  JsonArray stops = out["stops"].to<JsonArray>();
  for (const StopSnapshot &s : snap.stops) {
    JsonObject so = stops.add<JsonObject>();
    so["key"] = s.key;
    so["ok"] = s.ok;
    so["error"] = s.error;
    JsonArray arrivals = so["arrivals"].to<JsonArray>();
    for (const Arrival &a : s.arrivals) {
      serializeArrival(a, arrivals.add<JsonObject>(), now);
    }
  }
  JsonArray alerts = out["alerts"].to<JsonArray>();
  for (const Alert &al : snap.alerts) {
    JsonObject ao = alerts.add<JsonObject>();
    ao["route"] = al.route;
    ao["text"] = al.text;
    ao["current"] = al.current;
    JsonArray detours = ao["detours"].to<JsonArray>();
    for (const std::string &d : al.detours) {
      detours.add(d);
    }
  }
}

void sendJson(AsyncWebServerRequest *request, int code, JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  request->send(code, "application/json", body);
}

void sendError(AsyncWebServerRequest *request, int code, const std::string &message, const std::string &path = "") {
  JsonDocument doc;
  doc["error"] = message;
  if (!path.empty()) {
    doc["path"] = path;
  }
  sendJson(request, code, doc);
}

void sendNotImplemented(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["error"] = "not implemented in this firmware skeleton";
  doc["path"] = request->url().c_str();
  sendJson(request, 501, doc);
}

// Reboots shortly after the current request's response has had a chance to
// go out - calling ESP.restart() directly inside the handler risks cutting
// the HTTP response off mid-flight.
void scheduleRestart() {
  xTaskCreate(
    [](void *) {
      vTaskDelay(pdMS_TO_TICKS(300));
      ESP.restart();
    },
    "restart", 2048, nullptr, 1, nullptr
  );
}

void handleGetState(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["time"] = (int64_t)time(nullptr);
  doc["uptime"] = (uint32_t)(millis() / 1000);
  doc["heap"] = ESP.getFreeHeap();

  Config cfg = getActiveConfig();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();
  wifi["mdns"] = cfg.device.name + ".local";

  SdStatus sd = getSdStatus();
  JsonObject sdj = doc["sd"].to<JsonObject>();
  sdj["mounted"] = sd.mounted;
  sdj["free_mb"] = (double)sd.free_bytes / (1024.0 * 1024.0);
  sdj["log_bytes"] = logBytes();

  PollStatus poll = getPollStatus();
  JsonObject last_poll = doc["last_poll"].to<JsonObject>();
  last_poll["ok"] = poll.ok;
  // -1 (never polled yet) vs. a real, non-negative age in seconds.
  last_poll["age_s"] = poll.has_polled ? (int32_t)((uint32_t)time(nullptr) - poll.last_poll_epoch) : (int32_t)-1;
  last_poll["error"] = poll.last_error;

  // DESIGN.md SS3 task: "the demo snapshot serialized per DESIGN SS7" -
  // real arrivals await transit_core::merge() (see net_poller.cpp's TODO).
  // NB: doc.as<JsonObject>(), not .to<JsonObject>() - the latter clears the
  // document, which would wipe the time/uptime/heap/wifi/sd/last_poll
  // fields set above (doc's root is already an object at this point).
  Snapshot demo = buildDemoSnapshot((transit::Epoch)time(nullptr));
  serializeSnapshot(demo, doc.as<JsonObject>());

  sendJson(request, 200, doc);
}

void handleGetConfig(AsyncWebServerRequest *request) {
  Config cfg = getActiveConfig();
  JsonDocument doc;
  configToJson(cfg, doc);
  sendJson(request, 200, doc);
}

void handlePutConfig(AsyncWebServerRequest *request, JsonVariant &json, const std::function<void()> &onConfigChanged) {
  Config cfg;
  ConfigError err;
  if (!jsonToConfig(json, cfg, err)) {
    sendError(request, 400, err.message, err.path);
    return;
  }
  if (!saveConfig(cfg)) {
    sendError(request, 500, "failed to write config to LittleFS");
    return;
  }
  setActiveConfig(cfg);
  if (onConfigChanged) {
    onConfigChanged();
  }
  JsonDocument doc;
  configToJson(cfg, doc);
  sendJson(request, 200, doc);
}

void handlePostReboot(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(request, 200, doc);
  scheduleRestart();
}

void handlePostWifiReset(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(request, 200, doc);
  // Mirrors WiFiManager::resetSettings()'s own ESP32 path (WiFiManager.cpp
  // v2.0.17): enable STA, then erase credentials, then reboot so main.cpp's
  // WiFiManager::autoConnect() finds no saved network and opens the portal.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  scheduleRestart();
}

const char kPlaceholderHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Philly Transit Display</title>
<style>body{font:16px system-ui,sans-serif;margin:2rem;color:#222}code{background:#eee;padding:.1em .3em;border-radius:3px}</style>
</head><body>
<h1>Philly Transit Display</h1>
<p>The real web UI ships as a generated, gzipped bundle (<code>web/build.mjs</code> &rarr;
<code>firmware/src/generated/web_assets.h</code>) and hasn't been built into this firmware yet.</p>
<p>The device API already works - try <a href="/api/state">/api/state</a> or
<a href="/api/config">/api/config</a>.</p>
</body></html>
)HTML";

}  // namespace

void startWebServer(std::function<void()> onConfigChanged) {
  g_server.on("/api/state", HTTP_GET, handleGetState);
  g_server.on("/api/config", HTTP_GET, handleGetConfig);
  g_server.on("/api/config", HTTP_PUT, [onConfigChanged](AsyncWebServerRequest *request, JsonVariant &json) { handlePutConfig(request, json, onConfigChanged); });
  g_server.on("/api/reboot", HTTP_POST, handlePostReboot);
  g_server.on("/api/wifi/reset", HTTP_POST, handlePostWifiReset);

  // DESIGN.md SS7 routes not implemented in this skeleton.
  for (const char *path : {"/api/proxy/stops", "/api/proxy/schedule", "/api/rail/stations", "/api/stats", "/api/log/index", "/api/ota"}) {
    g_server.on(path, HTTP_GET, sendNotImplemented);
  }
  g_server.on("/api/ota", HTTP_POST, sendNotImplemented);
  // "/api/log/<file>.csv" - matched with a regex-free prefix check below via
  // onNotFound instead of a wildcard pattern, to keep the matcher simple.

  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { request->send_P(200, "text/html", kPlaceholderHtml); });

  g_server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->url().startsWith("/api/log/")) {
      sendNotImplemented(request);
      return;
    }
    request->send(404, "application/json", "{\"error\":\"not found\"}");
  });

  g_server.begin();
  log_i("web_server: listening on port 80");
}

}  // namespace transit_app
