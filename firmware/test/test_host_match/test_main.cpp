// Host tests for the DNS-rebinding Host check (DESIGN.md SS7, SS12.1; review F05).
//
// `pio test -e native -f test_host_match`. The check moved out of web_server.cpp into
// src/app/host_match.h for two reasons and this file is the second of them: it is the whole of a
// security control, its edges are fiddly (a port suffix, case folding, the `.local` alias, an
// empty or over-long value), and none of that needs a device to exercise. The first reason - that
// it must not allocate, because it now runs from `HostGuardHandler::canHandle()` where a
// std::bad_alloc would be std::terminate - is why every function below writes into a caller-owned
// buffer instead of returning a std::string.
#include <unity.h>

#include <cstring>

#include "../../src/app/host_match.h"

using transit_app::hostIsAlwaysAllowed;
using transit_app::hostNamesDevice;
using transit_app::kMaxHostChars;
using transit_app::normalizeHost;
using transit_app::storeHostName;

namespace {

// Normalise `raw` the way hostAllowed() does, into a fresh buffer, so each case reads as one line.
struct Normalized {
  char buf[kMaxHostChars + 1];
  size_t len;
  explicit Normalized(const char *raw) { len = normalizeHost(raw, std::strlen(raw), buf, sizeof buf); }
};

// The whole check, minus the IP comparison (which needs WiFi.localIP()): what web_server.cpp's
// hostAllowed() decides for a device called `device_name`.
bool allowed(const char *raw, const char *device_name) {
  Normalized n(raw);
  if (n.len == 0) return false;
  if (hostIsAlwaysAllowed(n.buf)) return true;
  return hostNamesDevice(n.buf, device_name);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_normalize_strips_the_port_and_folds_case(void) {
  TEST_ASSERT_EQUAL_STRING("transit-display", Normalized("Transit-Display").buf);
  TEST_ASSERT_EQUAL_STRING("transit-display", Normalized("transit-display:80").buf);
  TEST_ASSERT_EQUAL_STRING("transit-display", Normalized("TRANSIT-DISPLAY:8080").buf);
  TEST_ASSERT_EQUAL_STRING("192.168.1.181", Normalized("192.168.1.181:80").buf);
}

// A Host we cannot store is a Host that is not ours: saying no is the answer, not truncating to
// something that might then match.
void test_normalize_refuses_the_unusable(void) {
  TEST_ASSERT_EQUAL_UINT32(0, Normalized("").len);
  TEST_ASSERT_EQUAL_UINT32(0, Normalized(":80").len);  // nothing but a port
  char too_long[kMaxHostChars + 8];
  std::memset(too_long, 'a', sizeof too_long - 1);
  too_long[sizeof too_long - 1] = '\0';
  TEST_ASSERT_EQUAL_UINT32(0, Normalized(too_long).len);
  // kMaxHostChars characters is the longest value that still fits with its NUL, and it is
  // accepted: the cliff is one character further on, not one character early.
  char at_the_limit[kMaxHostChars + 1];
  std::memset(at_the_limit, 'a', kMaxHostChars);
  at_the_limit[kMaxHostChars] = '\0';
  TEST_ASSERT_EQUAL_UINT32(kMaxHostChars, Normalized(at_the_limit).len);
  char one_over[kMaxHostChars + 2];
  std::memset(one_over, 'a', kMaxHostChars + 1);
  one_over[kMaxHostChars + 1] = '\0';
  TEST_ASSERT_EQUAL_UINT32(0, Normalized(one_over).len);
}

void test_the_device_name_and_its_mdns_alias_are_accepted(void) {
  TEST_ASSERT_TRUE(allowed("transit-display", "transit-display"));
  TEST_ASSERT_TRUE(allowed("transit-display.local", "transit-display"));
  TEST_ASSERT_TRUE(allowed("Transit-Display.LOCAL:80", "transit-display"));
}

// The attack this exists to stop: a page on the public internet resolves its own name to the
// device's LAN address. The one thing it cannot forge is the Host header, which still carries the
// attacker's domain.
void test_a_foreign_host_is_refused(void) {
  TEST_ASSERT_FALSE(allowed("evil.example.com", "transit-display"));
  TEST_ASSERT_FALSE(allowed("evil.example.com:80", "transit-display"));
  TEST_ASSERT_FALSE(allowed("", "transit-display"));
}

// Prefix and suffix games around the real name, which is where a hand-rolled comparison goes wrong.
void test_near_misses_are_refused(void) {
  TEST_ASSERT_FALSE(allowed("transit-displa", "transit-display"));       // short
  TEST_ASSERT_FALSE(allowed("transit-displayx", "transit-display"));     // long
  TEST_ASSERT_FALSE(allowed("transit-display.locale", "transit-display"));
  TEST_ASSERT_FALSE(allowed("transit-display.local.evil.com", "transit-display"));
  TEST_ASSERT_FALSE(allowed("xtransit-display", "transit-display"));
  TEST_ASSERT_FALSE(allowed("transit-display.", "transit-display"));
}

void test_localhost_and_the_setup_ap_are_always_allowed(void) {
  TEST_ASSERT_TRUE(allowed("localhost", "transit-display"));
  TEST_ASSERT_TRUE(allowed("localhost:8080", "transit-display"));
  TEST_ASSERT_TRUE(allowed("192.168.4.1", "transit-display"));
  TEST_ASSERT_FALSE(allowed("localhost.evil.com", "transit-display"));
  TEST_ASSERT_FALSE(allowed("192.168.4.10", "transit-display"));
}

// Before the config has loaded there is no legitimate name to accept, so nothing matches by name.
// Accepting "" here would let every Host through in the window before setHostName() has run.
void test_an_empty_device_name_matches_nothing(void) {
  TEST_ASSERT_FALSE(hostNamesDevice("transit-display", ""));
  TEST_ASSERT_FALSE(hostNamesDevice("", ""));
  TEST_ASSERT_FALSE(hostNamesDevice(".local", ""));
  TEST_ASSERT_TRUE(allowed("localhost", ""));  // still reachable over an ssh tunnel
}

void test_store_host_name_folds_and_truncates(void) {
  char out[8];
  storeHostName("Transit", out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("transit", out);
  storeHostName("TransitDisplay", out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("transit", out);  // truncated, still NUL-terminated
  storeHostName(nullptr, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("", out);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_normalize_strips_the_port_and_folds_case);
  RUN_TEST(test_normalize_refuses_the_unusable);
  RUN_TEST(test_the_device_name_and_its_mdns_alias_are_accepted);
  RUN_TEST(test_a_foreign_host_is_refused);
  RUN_TEST(test_near_misses_are_refused);
  RUN_TEST(test_localhost_and_the_setup_ap_are_always_allowed);
  RUN_TEST(test_an_empty_device_name_matches_nothing);
  RUN_TEST(test_store_host_name_folds_and_truncates);
  return UNITY_END();
}
