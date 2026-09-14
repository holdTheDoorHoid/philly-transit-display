// SEPTA TLS trust anchor for http_fetch / net_poller.
//
// DESIGN.md SS2 and SS4 call for "HTTPS with Amazon Trust Services roots" and
// "the Arduino-ESP32 CA bundle mechanism". On Arduino-ESP32 core 3.x that
// mechanism is NetworkClientSecure::setCACertBundle(bundle, size)
// (WiFiClientSecure is a compat alias for NetworkClientSecure) - see
// ca_bundle_src/README.md for exactly how this was verified against the
// core's own source and why it needs an explicit bundle rather than a
// zero-config default.
#pragma once
#include <cstddef>
#include <cstdint>

namespace transit_app {

// A gen_crt_bundle.py-format certificate bundle (Espressif's compact
// "subject name + public key" encoding, NOT raw concatenated PEM/DER)
// containing the five Amazon Trust Services roots that sign
// www3.septa.org's certificate chain (see ca_bundle_src/README.md).
// Pass directly to NetworkClientSecure::setCACertBundle().
extern const uint8_t kSeptaCaBundle[];
extern const size_t kSeptaCaBundleLen;

}  // namespace transit_app
