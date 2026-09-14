// Streaming HTTPS GET helper. DESIGN.md SS5: "This is what transit_core's
// fetchers will use" - net_poller.cpp is the first caller; the GTFS-RT and
// BusSchedules/Alerts/Arrivals fetchers transit_core adds later should go
// through this too, so the retry policy and "never hold a TLS socket idle"
// rule (DESIGN.md SS4.7) live in exactly one place.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace transit_app {

// Performs one HTTPS GET of `url`, verifying the server certificate against
// kSeptaCaBundle (ca_bundle.h). Retries up to 3 times total, with backoff of
// 500ms/1000ms/2000ms between attempts, when:
//   - the connection/TLS handshake itself fails (get() would otherwise
//     return a negative HTTPClient error code - see <HTTPClient.h>'s
//     HTTPC_ERROR_* constants), or
//   - the server responds with 400, 501, 502, or 503 (DESIGN.md SS4.4:
//     SEPTA's BusSchedules endpoint intermittently 400s/501s on valid
//     stops; the same policy is applied here for every caller).
// Any other status - including a plain 404 - is returned immediately without
// retrying, on the theory that a client-side "this resource doesn't exist"
// response won't fix itself by asking again.
//
// `onData`, if non-null, is invoked with each chunk of the response body as
// it comes off the socket - the body is never buffered whole in RAM. Return
// false from `onData` to abort the transfer early (the connection is still
// closed cleanly before get() returns).
//
// Caveat: if a later attempt is needed after `onData` has already been
// called one or more times for an earlier, failed attempt, those earlier
// calls are NOT retracted - get() has no way to tell a streaming consumer
// "discard what you just saw." Callers that need all-or-nothing semantics
// should buffer nothing durable until get() returns, or reset their partial
// state whenever a new attempt starts (there is no per-attempt callback in
// this version - the whole call either fully succeeds after 1-3 attempts, or
// returns the final attempt's status/error).
//
// `tls_verify` selects whether the server certificate is checked against kSeptaCaBundle
// (config.device.tls_verify, DESIGN.md SS2/SS12: "verified by default"). Passing false calls
// NetworkClientSecure::setInsecure() instead and logs a one-time warning (net_poller.cpp is the
// only caller that can turn this off, driven by the live config) - repeated per-request warnings
// would be pointless log spam for something that's true for the device's whole uptime once set.
//
// Returns the HTTP status code (e.g. 200) from the response that was
// ultimately kept (the first non-retried one, or the last attempt if all 3
// were retried), or a negative HTTPClient error code if no attempt ever got
// a response at all.
int get(const char *url, std::function<bool(const uint8_t *, size_t)> onData, uint32_t timeout_ms,
        bool tls_verify = true);

// Selects the transport for every get(): with `use_https` false, an https:// URL is fetched over
// plain http:// with a NetworkClient and no TLS at all. Default false: a TLS session needs ~40 KB
// of heap with two 16 KB contiguous buffers, which the classic ESP32 (no PSRAM) running LVGL,
// Wi-Fi, and a web server cannot spare (measured 2026-09-14: 51 KB free, 14 KB largest block at
// poll time). SEPTA serves every endpoint over http:// without redirecting. config.device.use_https.
void setUseHttps(bool use_https);
bool useHttps();

}  // namespace transit_app
