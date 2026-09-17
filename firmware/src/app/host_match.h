// The DNS-rebinding Host check, as pure string work (DESIGN.md SS7, SS12; review F05).
//
// Split out of web_server.cpp for two reasons, both of which matter more than tidiness.
//
// 1. IT MUST NOT ALLOCATE. This check now runs from `HostGuardHandler::canHandle()`, which the
//    library calls at end-of-headers on the AsyncTCP task - inside `_parseLine`, with no `try`
//    anywhere above it. A `std::bad_alloc` escaping from there is `std::terminate`, i.e. a reboot,
//    exactly like the middleware-chain case the same pass removed. The old version built a
//    `std::string` from the header, lower-cased it, and compared it against `g_host_name +
//    ".local"` - two heap allocations on every single request, either of which could throw under
//    the memory pressure this firmware spends its life near. Everything below works in caller-owned
//    fixed buffers and cannot allocate, cannot throw, and cannot fail on a long input except by
//    saying no.
//
// 2. IT IS WORTH TESTING. It is the whole of a security control, it has fiddly edges (a port
//    suffix, case folding, the `.local` alias, an empty or over-long value), and none of that
//    needs a device. `pio test -e native -f test_host_match` covers it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace transit_app {

// Enough for any host this device can legitimately be reached by: `device.name` is validated at up
// to 32 characters (config_store.cpp `kMaxNameLen`), plus ".local" is 38, plus ":65535" is 44. A
// value that does not fit is not one of ours, so refusing it is the correct answer rather than a
// limitation.
constexpr size_t kMaxHostChars = 63;

// Lower-case `name` into `out` (NUL-terminated), truncating at `out_cap - 1`. Used to keep the
// configured device name in the form every comparison below wants.
inline void storeHostName(const char *name, char *out, size_t out_cap) {
  size_t i = 0;
  if (out_cap == 0) return;
  if (name != nullptr) {
    for (; name[i] != '\0' && i + 1 < out_cap; ++i) {
      const char c = name[i];
      out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
  }
  out[i] = '\0';
}

// Normalise a raw `Host` header value into `out`: strip an optional `:port`, fold to lower case.
// Returns the number of characters written, or 0 when the value is empty, over-long, or nothing but
// a port. ASCII-only folding on purpose - a host name with a non-ASCII byte in it is not one of the
// ways this device can be addressed, and `tolower()` on a negative char is undefined behaviour.
inline size_t normalizeHost(const char *raw, size_t raw_len, char *out, size_t out_cap) {
  if (out_cap == 0) return 0;
  out[0] = '\0';
  if (raw == nullptr || raw_len == 0 || raw_len >= out_cap) return 0;
  size_t n = 0;
  for (size_t i = 0; i < raw_len; ++i) {
    const char c = raw[i];
    if (c == ':') break;  // ":port" - IPv6 literals are not a way this device can be reached
    out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  out[n] = '\0';
  return n;
}

// Does an already-normalised `host` name a device called `device_name` (already lower-cased)?
// Accepts the bare name and the mDNS `<name>.local` alias, and nothing else. An empty device name
// matches nothing: before the config has loaded there is no legitimate name to accept, and
// accepting "" would let a request with an empty Host through.
inline bool hostNamesDevice(const char *host, const char *device_name) {
  if (host == nullptr || device_name == nullptr) return false;
  if (host[0] == '\0' || device_name[0] == '\0') return false;
  size_t i = 0;
  for (; device_name[i] != '\0'; ++i) {
    if (host[i] != device_name[i]) return false;
  }
  if (host[i] == '\0') return true;  // exactly the device name
  const char kLocal[] = ".local";
  for (size_t j = 0; j < sizeof(kLocal) - 1; ++j) {
    if (host[i + j] != kLocal[j]) return false;
  }
  return host[i + sizeof(kLocal) - 1] == '\0';
}

// The two host names that are always acceptable whatever the device is called: the loopback name a
// person reaches the device through over an ssh tunnel, and the fixed address of the setup AP.
// `192.168.4.1` is a literal rather than a comparison against the softAP's IP because it is fixed
// by the ESP-IDF's own AP defaults and the check has to work before that interface exists.
inline bool hostIsAlwaysAllowed(const char *host) {
  if (host == nullptr) return false;
  const char *const kAlways[] = {"localhost", "192.168.4.1"};
  for (const char *candidate : kAlways) {
    size_t i = 0;
    for (; candidate[i] != '\0' && host[i] == candidate[i]; ++i) {
    }
    if (candidate[i] == '\0' && host[i] == '\0') return true;
  }
  return false;
}

}  // namespace transit_app
