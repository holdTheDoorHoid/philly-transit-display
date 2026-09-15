#include "wifi_portal.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <vector>

#include "auth.h"
#include "status_led.h"
#include "ui/ui.h"

namespace transit_app {

namespace {

constexpr uint32_t kStoredCredsTimeoutMs = 20000;
constexpr uint32_t kConnectAttemptTimeoutMs = 20000;
constexpr uint32_t kPumpIntervalMs = 15;
// Retry backoff for a provisioned device whose network is not there (review F10). Starts fast for
// the common case - the router is rebooting and will be back in a few seconds - and settles at a
// minute so a device left running next to a dead router is not scanning continuously for days.
constexpr uint32_t kRetryStartMs = 5000;
constexpr uint32_t kRetryMaxMs = 60000;
// How long the setup AP stays up with nobody joined to it. The portal is the one moment this
// device is deliberately reachable by strangers, so it does not stay open indefinitely; while a
// client IS connected the timer is held off, because that client may be someone mid-setup.
constexpr uint32_t kPortalIdleTimeoutMs = 10 * 60 * 1000;

enum class ConnectState { Idle, Connecting, Connected, Failed };

DNSServer g_dns;
AsyncWebServer g_portal(80);

// g_state is written by the portal's pump loop (the main task) and read by handleStatus() on the
// async web server's task; the credentials are written by handleSave() on the async task and read
// by the pump loop. The enum is a single word and marked volatile; the two std::strings are not
// atomic in any sense, so they get a mutex (review F10: the old code handed a heap-allocated pair
// to a task it never checked the creation of, and wrote g_state from three places unsynchronised).
volatile ConnectState g_state = ConnectState::Idle;
SemaphoreHandle_t g_creds_mutex = nullptr;
std::string g_creds_ssid;
std::string g_creds_pass;
bool g_creds_pending = false;

SemaphoreHandle_t credsMutex() {
  if (g_creds_mutex == nullptr) g_creds_mutex = xSemaphoreCreateMutex();
  return g_creds_mutex;
}

// The SSID the ESP-IDF Wi-Fi driver has persisted, or "" if the device has never been
// provisioned. WiFi.SSID() only answers while associated, which is exactly the case this needs to
// distinguish, so read the stored station config directly.
std::string storedSsid() {
  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return "";
  char buf[33];
  memcpy(buf, conf.sta.ssid, 32);
  buf[32] = '\0';
  return std::string(buf);
}

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
  let r = await fetch('/save', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)});
  if (!r.ok) { let e = await r.json().catch(function(){return {}});
    document.getElementById('msg').textContent = e.error || ('Rejected (HTTP ' + r.status + ')'); return; }
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

  // Built with ArduinoJson rather than string concatenation: an SSID is arbitrary bytes chosen by
  // whoever is nearby, and a neighbour whose network is called `","x":"` used to break the
  // portal's JSON outright - the list would not render and the owner could not get past setup.
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &f : found) arr.add(f.second);
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

// 802.11 / WPA2 limits, checked here so a bad value is a 400 rather than an esp_wifi error
// several seconds later that the page reports as "could not connect".
bool validCredentials(const std::string &ssid, const std::string &pass, std::string &error) {
  if (ssid.empty() || ssid.size() > 32) {
    error = "network name must be 1-32 characters";
    return false;
  }
  if (!pass.empty() && (pass.size() < 8 || pass.size() > 63)) {
    error = "password must be empty (open network) or 8-63 characters";
    return false;
  }
  for (const std::string *s : {&ssid, &pass}) {
    for (char c : *s) {
      unsigned char u = (unsigned char)c;
      if (u < 0x20 || u == 0x7F) {
        error = "network name and password must not contain control characters";
        return false;
      }
    }
  }
  return true;
}

void handleSave(AsyncWebServerRequest *request) {
  std::string ssid = request->hasParam("ssid", true) ? request->getParam("ssid", true)->value().c_str() : "";
  std::string pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value().c_str() : "";
  std::string error;
  if (!validCredentials(ssid, pass, error)) {
    request->send(400, "application/json", String("{\"error\":\"") + error.c_str() + "\"}");
    return;
  }
  // One attempt at a time: WiFi.begin() while an attempt is in flight tears the first one down
  // and the two /status.json pollers then disagree about which network is being joined.
  if (g_state == ConnectState::Connecting) {
    request->send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  SemaphoreHandle_t mutex = credsMutex();
  if (mutex == nullptr || xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  g_creds_ssid = ssid;
  g_creds_pass = pass;
  g_creds_pending = true;
  xSemaphoreGive(mutex);
  g_state = ConnectState::Connecting;
  request->send(200, "application/json", "{\"ok\":true}");
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

void startPortal(const std::string &ap_name, const std::string &ap_pass) {
  WiFi.mode(WIFI_AP_STA);
  // WPA2, not open (review F02). An open setup AP means anyone in radio range can join it and
  // watch the owner type their home Wi-Fi password into the portal - the one secret this device
  // handles that is not its own. The password is generated per device and displayed on the panel
  // next to a QR code (ui.cpp), so it costs the owner nothing.
  WiFi.softAP(ap_name.c_str(), ap_pass.c_str());
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

// Pumps LVGL until Wi-Fi is up or `timeout_ms` has passed. Returns true if connected. Sets
// `tapped` if the screen was touched while waiting, so the caller can drop into the portal.
bool waitForConnection(uint32_t timeout_ms, const std::function<void()> &pump, bool *tapped) {
  uint32_t deadline = millis() + timeout_ms;
  while ((int32_t)(millis() - deadline) < 0) {
    if (WiFi.status() == WL_CONNECTED) return true;
    pump();
    if (tapped != nullptr && ui::consumeTap()) {
      *tapped = true;
      return false;
    }
    delay(kPumpIntervalMs);
  }
  return WiFi.status() == WL_CONNECTED;
}

// The portal's own event loop: LVGL, the captive DNS server, the /save connection attempt (as a
// small state machine rather than a second task - see the header) and the idle timeout. Never
// returns: it either reboots into a connected device or reboots back into the retry loop.
[[noreturn]] void runPortal(const std::function<void()> &pump) {
  uint32_t last_client_ms = millis();
  bool attempting = false;
  uint32_t attempt_deadline = 0;
  std::string attempt_ssid;

  for (;;) {
    pump();
    delay(kPumpIntervalMs);

    if (!attempting) {
      bool start = false;
      SemaphoreHandle_t mutex = credsMutex();
      if (g_creds_pending && mutex != nullptr && xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (g_creds_pending) {
          attempt_ssid = g_creds_ssid;
          WiFi.begin(g_creds_ssid.c_str(), g_creds_pass.c_str());
          g_creds_pending = false;
          start = true;
        }
        xSemaphoreGive(mutex);
      }
      if (start) {
        attempting = true;
        attempt_deadline = millis() + kConnectAttemptTimeoutMs;
        log_i("wifi_portal: trying '%s'", attempt_ssid.c_str());
      }
    } else if (WiFi.status() == WL_CONNECTED) {
      g_state = ConnectState::Connected;
      log_i("wifi_portal: connected to '%s', rebooting", attempt_ssid.c_str());
      // Long enough for the page's next /status.json poll to see "connected" before the reboot.
      uint32_t until = millis() + 1200;
      while ((int32_t)(millis() - until) < 0) {
        pump();
        delay(kPumpIntervalMs);
      }
      ESP.restart();
    } else if ((int32_t)(millis() - attempt_deadline) >= 0) {
      log_w("wifi_portal: could not connect to '%s'", attempt_ssid.c_str());
      g_state = ConnectState::Failed;
      attempting = false;
    }

    if (WiFi.softAPgetStationNum() > 0 || attempting) {
      last_client_ms = millis();  // someone is here; hold the timeout off
    } else if ((int32_t)(millis() - last_client_ms) >= (int32_t)kPortalIdleTimeoutMs) {
      log_w("wifi_portal: setup AP idle for 10 minutes, rebooting into the retry loop");
      ESP.restart();
    }
  }
}

}  // namespace

void connectWifiOrPortal(const std::string &ap_name, const std::function<void()> &pump) {
  setStatusLed(LedState::Connecting);
  WiFi.mode(WIFI_STA);
  // First call in the boot sequence, and deliberately here rather than in setup(): the radio is
  // now on, which is what makes esp_random() a true RNG, and a blank device generates its one and
  // only admin PIN and setup-AP password inside begin(). Prints the PIN over serial.
  auth::begin();
  std::string stored = storedSsid();
  bool tapped = false;

  if (!stored.empty()) {
    // Reconnect with whatever the ESP-IDF Wi-Fi driver has persisted in NVS.
    WiFi.begin();
    if (waitForConnection(kStoredCredsTimeoutMs, pump, nullptr)) {
      setStatusLed(LedState::Off);
      log_i("wifi_portal: connected with stored credentials, IP %s", WiFi.localIP().toString().c_str());
      return;
    }

    // Review F10: a provisioned device that cannot reach its network must NOT open the setup AP
    // on its own. Doing that turns every router reboot, every power cut and every out-of-range
    // moment into a window where an open (now WPA2, but still) AP named after this device is on
    // the air, the owner's saved credentials are one /save away from being overwritten, and the
    // display is showing a setup screen instead of arrivals. So it keeps retrying the network it
    // knows, forever, and only a deliberate touch on the panel opens the portal.
    uint32_t backoff_ms = kRetryStartMs;
    while (!tapped) {
      ui::showConnectingScreen(stored, "waiting for the network");
      WiFi.disconnect(false /* keep the stored credentials */);
      WiFi.begin();
      if (waitForConnection(kConnectAttemptTimeoutMs, pump, &tapped)) {
        setStatusLed(LedState::Off);
        log_i("wifi_portal: reconnected to '%s', IP %s", stored.c_str(), WiFi.localIP().toString().c_str());
        return;
      }
      if (tapped) break;

      char detail[64];
      snprintf(detail, sizeof(detail), "retrying in %u s", (unsigned)(backoff_ms / 1000));
      ui::showConnectingScreen(stored, detail);
      uint32_t until = millis() + backoff_ms;
      while ((int32_t)(millis() - until) < 0) {
        pump();
        if (ui::consumeTap()) {
          tapped = true;
          break;
        }
        delay(kPumpIntervalMs);
      }
      backoff_ms = std::min(backoff_ms * 2, kRetryMaxMs);
    }
    log_w("wifi_portal: screen tapped during reconnect; opening the setup AP");
  } else {
    log_w("wifi_portal: no stored Wi-Fi credentials; opening the setup AP");
  }

  ui::showWifiSetupScreen(ap_name, auth::apPassword());
  startPortal(ap_name, auth::apPassword());
  runPortal(pump);
}

}  // namespace transit_app
