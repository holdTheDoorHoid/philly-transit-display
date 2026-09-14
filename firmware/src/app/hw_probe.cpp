#include "hw_probe.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_mac.h>
#include <lvgl.h>

#ifndef BOARD_NAME
#define BOARD_NAME "unknown-env"
#endif

namespace transit_app {

namespace {

constexpr int kGt911Rst = 25;  // per boards/esp32-3248S035C.json
constexpr int kGt911Int = 21;

// Scans one I2C bus for responders and reports them. Returns the count found.
int scanPair(int sda, int scl) {
  int found = 0;
  if (!Wire.begin(sda, scl, 100000)) {
    Serial.printf("[probe] i2c sda=%d scl=%d: Wire.begin failed\n", sda, scl);
    return 0;
  }
  Wire.setTimeOut(20);
  String addrs;
  for (uint8_t a = 0x08; a < 0x78; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), " 0x%02X", a);
      addrs += buf;
      ++found;
    }
  }
  Wire.end();
  pinMode(sda, INPUT);
  pinMode(scl, INPUT);
  Serial.printf("[probe] i2c sda=%d scl=%d: %d device(s)%s\n", sda, scl, found, found ? addrs.c_str() : "");
  return found;
}

const char *touchGuess(bool has5d, bool has14, bool has15) {
  if (has5d || has14) return "GT911 capacitive";
  if (has15) return "CST816S/CST820 capacitive";
  return "none on I2C -> XPT2046 resistive (SPI)";
}

}  // namespace

void hwProbeEarly() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("[probe] env=%s firmware=%s\n", BOARD_NAME, FIRMWARE_VERSION);
  Serial.printf("[probe] chip=%s rev=%u cores=%u cpu=%uMHz\n", ESP.getChipModel(), (unsigned)ESP.getChipRevision(), (unsigned)ESP.getChipCores(), (unsigned)ESP.getCpuFreqMHz());
  Serial.printf("[probe] flash=%u bytes psram=%u bytes\n", (unsigned)ESP.getFlashChipSize(), (unsigned)ESP.getPsramSize());
  Serial.printf("[probe] heap free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  {
    // The LVGL draw buffer is the largest single allocation at boot; report whether the
    // sizes the board definition implies would even fit (DESIGN.md SS5 memory rules).
    const size_t sizes[] = {115200, 76800, 30720};
    for (size_t n : sizes) {
      void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      Serial.printf("[probe] heap_caps_malloc(%u, INTERNAL|8BIT) -> %s\n", (unsigned)n, p ? "ok" : "NULL");
      free(p);
    }
  }
  Serial.printf("[probe] mac=%02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // A GT911 only answers after its reset line is released; INT low during
  // reset selects address 0x5D. On a resistive board these pins are the
  // touch chip-select / unused, so a brief pulse is harmless.
  pinMode(kGt911Int, OUTPUT);
  digitalWrite(kGt911Int, LOW);
  pinMode(kGt911Rst, OUTPUT);
  digitalWrite(kGt911Rst, LOW);
  delay(10);
  digitalWrite(kGt911Rst, HIGH);
  delay(60);
  pinMode(kGt911Int, INPUT);

  bool has5d = false, has14 = false, has15 = false;
  const int pairs[2][2] = {{33, 32}, {21, 22}};
  for (auto &p : pairs) {
    if (!Wire.begin(p[0], p[1], 100000)) continue;
    Wire.setTimeOut(20);
    Wire.beginTransmission(0x5D); has5d |= Wire.endTransmission() == 0;
    Wire.beginTransmission(0x14); has14 |= Wire.endTransmission() == 0;
    Wire.beginTransmission(0x15); has15 |= Wire.endTransmission() == 0;
    Wire.end();
    scanPair(p[0], p[1]);
  }
  pinMode(kGt911Rst, INPUT);
  Serial.printf("[probe] touch guess: %s\n", touchGuess(has5d, has14, has15));
}

void hwProbeDisplay() {
  lv_display_t *d = lv_display_get_default();
  if (!d) {
    Serial.println("[probe] lvgl display: none");
    return;
  }
  Serial.printf("[probe] lvgl display %dx%d rotation=%d dpi=%d\n", (int)lv_display_get_horizontal_resolution(d), (int)lv_display_get_vertical_resolution(d), (int)lv_display_get_rotation(d), (int)lv_display_get_dpi(d));
}

}  // namespace transit_app
