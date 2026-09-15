#include "auth.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>

#include <cstring>

namespace transit_app::auth {

namespace {

constexpr const char *kNamespace = "ptd";
constexpr const char *kPinKey = "pin";
constexpr const char *kApPassKey = "ap_pass";

// Ten characters, and an alphabet with no 0/O/1/l/I in it: the setup password is read off a
// 320 px panel across the room and typed into a phone, so a character the owner can misread is a
// support call. 57 symbols ^ 10 is ~5.7e17 combinations, far past anything WPA2 handshake
// cracking reaches before the portal's ten-minute idle timeout closes the AP (review F02/F10).
constexpr const char kApAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr size_t kApAlphabetLen = sizeof(kApAlphabet) - 1;
constexpr size_t kApPassLen = 10;

std::string g_pin;
std::string g_ap_pass;
bool g_begun = false;

int g_wrong_streak = 0;
uint32_t g_locked_until_ms = 0;

// esp_random() is only a *true* RNG while the RF subsystem is powered: ESP-IDF is explicit that
// without Wi-Fi or Bluetooth running, the hardware RNG's entropy source is limited and its output
// "should not be considered truly random". Both secrets here are generated exactly once, on the
// first boot of a blank device, and then live in NVS forever - so begin() is deliberately called
// after WiFi.mode(WIFI_STA) has brought the radio up (see wifi_portal.cpp) rather than early in
// setup(), where the one PIN this device will ever have would be drawn from a weak source.
//
// Rejection sampling keeps every 6-digit value exactly equally likely: `esp_random() % 1000000`
// is not uniform, and a PIN with a measurable skew is a silly thing to ship when the fix is two
// lines.
std::string randomPin() {
  constexpr uint32_t kLimit = 4294000000u;  // largest multiple of 1e6 that fits in uint32_t
  uint32_t r;
  do {
    r = esp_random();
  } while (r >= kLimit);
  char buf[8];
  snprintf(buf, sizeof(buf), "%06u", (unsigned)(r % 1000000u));
  return std::string(buf);
}

std::string randomApPassword() {
  // Same reasoning as randomPin(): reject the tail of the byte range that would over-represent
  // the first (256 % 57) symbols of the alphabet.
  const uint8_t kAcceptBelow = (uint8_t)(256u - (256u % kApAlphabetLen));
  std::string out;
  out.reserve(kApPassLen);
  while (out.size() < kApPassLen) {
    uint32_t word = esp_random();
    for (int i = 0; i < 4 && out.size() < kApPassLen; ++i) {
      uint8_t b = (uint8_t)(word >> (8 * i));
      if (b >= kAcceptBelow) continue;
      out.push_back(kApAlphabet[b % kApAlphabetLen]);
    }
  }
  return out;
}

// Length-independent comparison: always walks kMaxPinLen bytes and folds the length difference
// into the same accumulator, so the time taken says nothing about how many leading characters of
// a guess were right (review F01). Short-circuiting strcmp() on a LAN is not a practical oracle,
// but constant time costs nothing here.
bool constantTimeEquals(const char *a, size_t a_len, const char *b, size_t b_len) {
  uint32_t diff = (uint32_t)(a_len ^ b_len);
  for (size_t i = 0; i < kMaxPinLen; ++i) {
    uint8_t ca = i < a_len ? (uint8_t)a[i] : 0;
    uint8_t cb = i < b_len ? (uint8_t)b[i] : 0;
    diff |= (uint32_t)(ca ^ cb);
  }
  return diff == 0;
}

Preferences &prefs() {
  static Preferences p;
  return p;
}

}  // namespace

void begin() {
  if (g_begun) return;
  g_begun = true;

  if (!prefs().begin(kNamespace, false /* read-write */)) {
    // NVS is unavailable (a partition table without an "nvs" entry, or a corrupted one). Fall
    // back to a PIN that lives only in RAM: the API stays protected for this boot and the serial
    // line still tells the owner what it is, rather than silently leaving everything open.
    log_e("auth: could not open NVS namespace '%s'; using a boot-only PIN", kNamespace);
    g_pin = randomPin();
    g_ap_pass = randomApPassword();
  } else {
    g_pin = std::string(prefs().getString(kPinKey, "").c_str());
    std::string ignored;
    if (!validPin(g_pin, ignored)) {
      g_pin = randomPin();
      prefs().putString(kPinKey, g_pin.c_str());
      log_w("auth: generated a new web PIN (none stored, or the stored one was unusable)");
    }
    g_ap_pass = std::string(prefs().getString(kApPassKey, "").c_str());
    if (g_ap_pass.size() < 8 || g_ap_pass.size() > 63) {  // WPA2 passphrase limits
      g_ap_pass = randomApPassword();
      prefs().putString(kApPassKey, g_ap_pass.c_str());
      log_w("auth: generated a new setup-AP password");
    }
  }

  // The recovery path (auth.h): serial requires physical access to the display, so printing the
  // PIN here is not a disclosure - it is how the owner learns it. Plain Serial.printf, not log_i:
  // CORE_DEBUG_LEVEL is 1 and this line must survive that (platformio.ini).
  Serial.printf("[auth] web PIN: %s\n", g_pin.c_str());
}

const std::string &pin() { return g_pin; }

const std::string &apPassword() { return g_ap_pass; }

Check check(const char *provided) {
  Check out;
  uint32_t now = millis();
  if (g_locked_until_ms != 0) {
    int32_t left = (int32_t)(g_locked_until_ms - now);
    if (left > 0) {
      out.result = Result::Locked;
      out.retry_s = (uint32_t)((left + 999) / 1000);
      return out;
    }
    g_locked_until_ms = 0;
    g_wrong_streak = 0;
  }

  if (provided == nullptr) {
    // A missing header is a client that has not been told about the PIN yet, not a guess: it does
    // not count towards the lockout, so an unaware script cannot lock the owner out.
    out.result = Result::Missing;
    return out;
  }

  if (constantTimeEquals(provided, strlen(provided), g_pin.c_str(), g_pin.size())) {
    g_wrong_streak = 0;
    out.result = Result::Ok;
    return out;
  }

  if (++g_wrong_streak >= kMaxWrongAttempts) {
    g_locked_until_ms = now + kLockoutSeconds * 1000u;
    if (g_locked_until_ms == 0) g_locked_until_ms = 1;  // millis() wrap: 0 means "not locked"
    out.result = Result::Locked;
    out.retry_s = kLockoutSeconds;
    log_w("auth: %d wrong PINs in a row; locked for %u s", g_wrong_streak, (unsigned)kLockoutSeconds);
    return out;
  }
  out.result = Result::Wrong;
  return out;
}

bool validPin(const std::string &next, std::string &error) {
  if (next.size() < kMinPinLen || next.size() > kMaxPinLen) {
    error = "pin must be " + std::to_string(kMinPinLen) + "-" + std::to_string(kMaxPinLen) + " characters";
    return false;
  }
  for (char c : next) {
    // Printable ASCII only, and no whitespace: the PIN travels in an HTTP header, where a space,
    // tab or newline would either be folded by an intermediary or split the header outright.
    unsigned char u = (unsigned char)c;
    if (u < 0x21 || u > 0x7E) {
      error = "pin must be printable ASCII with no spaces";
      return false;
    }
  }
  return true;
}

bool setPin(const std::string &next, std::string &error) {
  if (!validPin(next, error)) return false;
  if (prefs().putString(kPinKey, next.c_str()) == 0) {
    error = "could not store the new pin";
    return false;
  }
  g_pin = next;
  g_wrong_streak = 0;
  g_locked_until_ms = 0;
  log_w("auth: web PIN changed");
  return true;
}

}  // namespace transit_app::auth
