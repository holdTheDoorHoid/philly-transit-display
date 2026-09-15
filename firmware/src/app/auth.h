// Per-device admin PIN and setup-AP password (DESIGN.md SS7/SS12; adversarial review F01/F02).
//
// WHY a PIN and not a password/session: the state-changing half of the HTTP API (config, reboot,
// Wi-Fi reset, OTA, the tap test hook, log downloads) is what an attacker on the LAN can use to
// hurt the owner; *viewing* arrivals is harmless and stays open so the display keeps working like
// an appliance. A six-digit PIN in a custom header is the smallest thing that stops a drive-by
// request and it needs no session state on a device with ~75 KB of free heap.
//
// WHY NVS and not /config.json: GET /api/config is an open endpoint (the web UI reads it without
// authenticating), so the secret must live somewhere that endpoint never serializes. The
// Preferences library's "ptd" namespace is separate from the ESP-IDF Wi-Fi driver's own NVS
// entries and survives an OTA (only a full flash erase clears it).
//
// Recovery path: the PIN is printed once on the serial console at boot and shown on the device
// info screen. Both require physical possession of the display, which is the owner's intended
// fallback - there is no network-reachable way to read or reset it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace transit_app::auth {

// Loads the PIN and the setup-AP password from NVS, generating either one with esp_random() if it
// is missing or unusable, and prints "[auth] web PIN: 123456" on the serial console.
//
// Call this only once the Wi-Fi radio has been brought up (WiFi.mode()), never earlier: these two
// secrets are generated exactly once in a device's life, and esp_random() is only a true RNG
// while RF is powered (see the comment on randomPin() in auth.cpp). wifi_portal.cpp makes the
// first call, right after it starts the radio, so both the portal path and the normal boot are
// covered; main.cpp calls it again after Wi-Fi settles. Idempotent - later calls do nothing.
void begin();

// The current admin PIN ("123456"). Empty only if begin() has not run yet.
const std::string &pin();

// The WPA2 password of the first-time-setup SoftAP (review F02: an open AP hands anyone in range
// the credentials of the owner's home network as they are typed into the portal). Ten characters
// from an alphabet with no 0/O/1/l/I, so it can be read off the screen and typed without guessing.
const std::string &apPassword();

enum class Result {
  Ok,       // the PIN matched
  Missing,  // no X-Pin header at all              -> 401 {"error":"pin required"}
  Wrong,    // header present, wrong value         -> 401 {"error":"wrong pin"}
  Locked,   // too many wrong attempts in a row    -> 429 {"error":"too many attempts","retry_s":N}
};

struct Check {
  Result result = Result::Missing;
  uint32_t retry_s = 0;  // seconds left on the lockout; only meaningful for Result::Locked
};

// Compares `provided` (the X-Pin header value, or nullptr when the header is absent) against the
// stored PIN in constant time and applies the brute-force guard: kMaxWrongAttempts consecutive
// wrong PINs lock every protected endpoint for kLockoutSeconds; a correct PIN clears the counter.
//
// Thread-safety: every HTTP handler in this firmware runs on the single AsyncTCP task, so the
// counters below need no lock. Do not call this from another task without adding one.
Check check(const char *provided);

// Rules for a new PIN (POST /api/pin): 4-32 printable ASCII characters, no whitespace. Returns
// false and fills `error` with the message that goes out as the 400 body if `next` breaks them.
bool validPin(const std::string &next, std::string &error);

// Stores a new PIN in NVS and makes it current. Returns false (with `error` filled) if `next` is
// invalid or NVS refuses the write; the old PIN stays in effect in that case.
bool setPin(const std::string &next, std::string &error);

constexpr size_t kMinPinLen = 4;
constexpr size_t kMaxPinLen = 32;
constexpr int kMaxWrongAttempts = 5;
constexpr uint32_t kLockoutSeconds = 30;

}  // namespace transit_app::auth
