// Host stand-in for Arduino-ESP32's <WiFi.h> (firmware/sim). The screens only read connection
// state, RSSI, SSID and IP, and the device page's hold-to-reset gesture calls mode()/disconnect().
// The simulator sets the sim::wifi_* knobs per render to show the 0..4 bar states.
#pragma once
#include <string>

#include "Arduino.h"

namespace sim {
extern bool wifi_connected;
extern int wifi_rssi;
extern std::string wifi_ssid;
extern std::string wifi_ip;
}  // namespace sim

enum SimWlStatus { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
enum SimWifiMode { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2 };

struct SimIPAddress {
  std::string s;
  std::string toString() const { return s; }
};

struct SimWiFi {
  int status() { return sim::wifi_connected ? WL_CONNECTED : WL_DISCONNECTED; }
  int RSSI() { return sim::wifi_rssi; }
  std::string SSID() { return sim::wifi_ssid; }
  SimIPAddress localIP() { return {sim::wifi_ip}; }
  void mode(int) {}
  void disconnect(bool, bool) { std::printf("[sim] WiFi.disconnect() called (ignored)\n"); }
};
extern SimWiFi WiFi;
