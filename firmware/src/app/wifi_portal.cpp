#include "wifi_portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <vector>

#include "status_led.h"
#include "ui/ui.h"

namespace transit_app {

namespace {

constexpr uint32_t kStoredCredsTimeoutMs = 20000;
constexpr uint32_t kConnectAttemptTimeoutMs = 20000;
constexpr uint32_t kPumpIntervalMs = 15;

enum class ConnectState { Idle, Connecting, Connected, Failed };

DNSServer g_dns;
AsyncWebServer g_portal(80);
volatile ConnectState g_state = ConnectState::Idle;

// Tiny inline page: no framework, no external assets (flash budget - DESIGN.md SS2/SS10 apply
// the same "no bundler, no CDN for anything load-bearing" spirit here). JS polls /scan.json once
// to populate the SSID list and /status.json after a save to report progress.
const char kPortalHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Transit Display setup</title>
<style>
body{font:16px system-ui,sans-serif;margin:1.5rem;color:#222;background:#fafafa}
h1{font-size:1.3rem}
select,input,button{font-size:1rem;padding:.5em;width:100%;margin:.35em 0;box-sizing:border-box}
button{background:#274b8f;color:#fff;border:0;border-radius:4px}
#msg{margin-top:1em;min-height:1.4em;color:#444}
</style></head>
<body>
<h1>Wi-Fi setup</h1>
<p>Pick your home network and enter its password. The display will reboot once connected.</p>
<select id="ssid"><option>Scanning&hellip;</option></select>
<input id="pass" type="password" placeholder="Wi-Fi password">
<button onclick="save()">Connect</button>
<div id="msg"></div>
<script>
async function scan(tries){
  tries = tries || 0;
  let r = await fetch('/scan.json'); let j = await r.json();
  let sel = document.getElementById('ssid');
  if (!j.length && tries < 8) { setTimeout(function(){scan(tries+1)}, 1000); return; }
  sel.innerHTML = '';
  for (const s of j) { let o = document.createElement('option'); o.textContent = s; sel.appendChild(o); }
  if (!j.length) { let o = document.createElement('option'); o.textContent='(type SSID below)'; sel.appendChild(o); }
  let manual = document.createElement('input'); manual.id='ssidManual'; manual.placeholder='...or type SSID manually';
  sel.parentNode.insertBefore(manual, sel.nextSibling);
}
async function save(){
  document.getElementById('msg').textContent = 'Connecting…';
  let manual = document.getElementById('ssidManual');
  let ssid = (manual && manual.value) ? manual.value : document.getElementById('ssid').value;
  let pass = document.getElementById('pass').value;
  await fetch('/save', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)});
  poll();
}
async function poll(){
  let r = await fetch('/status.json'); let j = await r.json();
  let m = document.getElementById('msg');
  if (j.state === 'connected') { m.textContent = 'Connected! Rebooting…'; return; }
  if (j.state === 'failed') { m.textContent = 'Could not connect. Check the password and try again.'; return; }
  m.textContent = 'Connecting…';
  setTimeout(poll, 1000);
}
scan();
</script>
</body></html>
)HTML";

void sendPortalPage(AsyncWebServerRequest *request) {
  request->send(200, "text/html", kPortalHtml);
}

void handleScan(AsyncWebServerRequest *request) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true /* async */);
    n = 0;
  } else if (n == WIFI_SCAN_RUNNING) {
    n = 0;
  }
  std::vector<std::pair<int32_t, String>> found;
  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    bool dup = false;
    for (auto &f : found) {
      if (f.second == ssid) dup = true;
    }
    if (!dup) found.push_back({WiFi.RSSI(i), ssid});
  }
  std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.first > b.first; });
  if (n > 0) WiFi.scanDelete();

  String body = "[";
  for (size_t i = 0; i < found.size(); ++i) {
    if (i) body += ",";
    body += "\"" + found[i].second + "\"";
  }
  body += "]";
  request->send(200, "application/json", body);
}

void connectAttemptTask(void *arg) {
  auto *creds = static_cast<std::pair<std::string, std::string> *>(arg);
  WiFi.begin(creds->first.c_str(), creds->second.c_str());
  uint32_t deadline = millis() + kConnectAttemptTimeoutMs;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (WiFi.status() == WL_CONNECTED) {
    g_state = ConnectState::Connected;
    log_i("wifi_portal: connected to '%s', rebooting", creds->first.c_str());
    delete creds;
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP.restart();
  } else {
    log_w("wifi_portal: could not connect to '%s'", creds->first.c_str());
    g_state = ConnectState::Failed;
    delete creds;
  }
  vTaskDelete(nullptr);
}

void handleSave(AsyncWebServerRequest *request) {
  std::string ssid = request->hasParam("ssid", true) ? request->getParam("ssid", true)->value().c_str() : "";
  std::string pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value().c_str() : "";
  if (ssid.empty()) {
    request->send(400, "application/json", "{\"error\":\"ssid required\"}");
    return;
  }
  g_state = ConnectState::Connecting;
  request->send(200, "application/json", "{\"ok\":true}");
  auto *creds = new std::pair<std::string, std::string>(ssid, pass);
  xTaskCreate(connectAttemptTask, "wifi_connect", 4096, creds, 1, nullptr);
}

void handleStatus(AsyncWebServerRequest *request) {
  const char *s = "connecting";
  switch (g_state) {
    case ConnectState::Idle:
      s = "idle";
      break;
    case ConnectState::Connecting:
      s = "connecting";
      break;
    case ConnectState::Connected:
      s = "connected";
      break;
    case ConnectState::Failed:
      s = "failed";
      break;
  }
  String body = String("{\"state\":\"") + s + "\"}";
  request->send(200, "application/json", body);
}

void startPortal(const std::string &ap_name) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_name.c_str());
  delay(100);  // let the AP interface come up before starting DNS/HTTP on it
  WiFi.scanNetworks(true /* async */);
  g_dns.start();  // defaults: port 53, any domain, the SoftAP's own IP (192.168.4.1)

  g_portal.on("/", HTTP_GET, sendPortalPage);
  g_portal.on("/scan.json", HTTP_GET, handleScan);
  g_portal.on("/save", HTTP_POST, handleSave);
  g_portal.on("/status.json", HTTP_GET, handleStatus);
  // Most OSes probe a handful of well-known paths to detect a captive portal; serving the setup
  // page for anything unmatched makes that probe pop the "sign in to network" prompt.
  g_portal.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_GET) {
      sendPortalPage(request);
    } else {
      request->send(404);
    }
  });
  g_portal.begin();

  log_i("wifi_portal: SoftAP '%s' up at %s", ap_name.c_str(), WiFi.softAPIP().toString().c_str());
}

}  // namespace

void connectWifiOrPortal(const std::string &ap_name, const std::function<void()> &pump) {
  setStatusLed(LedState::Connecting);
  WiFi.mode(WIFI_STA);
  // Reconnect with whatever the ESP-IDF Wi-Fi driver has persisted in NVS, if any. With no
  // stored SSID the core fails immediately (ESP_ERR_WIFI_SSID, WL_CONNECT_FAILED), so do not
  // sit through the timeout in that case.
  wl_status_t began = WiFi.begin();
  uint32_t deadline = millis() + (began == WL_CONNECT_FAILED ? 0 : kStoredCredsTimeoutMs);
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
    pump();
    delay(kPumpIntervalMs);
  }
  if (WiFi.status() == WL_CONNECTED) {
    setStatusLed(LedState::Off);
    log_i("wifi_portal: connected with stored credentials, IP %s", WiFi.localIP().toString().c_str());
    return;
  }

  log_w("wifi_portal: no usable stored Wi-Fi; opening setup AP '%s' at 192.168.4.1", ap_name.c_str());
  ui::showWifiSetupScreen(ap_name);
  startPortal(ap_name);

  for (;;) {
    pump();
    delay(kPumpIntervalMs);
  }
}

}  // namespace transit_app
