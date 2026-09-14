// Streaming HTTPS GET helper. DESIGN.md SS5: "This is what transit_core's
// fetchers will use" - net_poller.cpp is the first caller; the GTFS-RT and
// BusSchedules/Alerts/Arrivals fetchers transit_core adds later should go
// through this too, so the retry policy and "never hold a TLS socket idle"
// rule (DESIGN.md SS4.7) live in exactly one place.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace transit_app {

// Response headers a caller may want back from get(). SEPTA's BusSchedules is served by an AWS
// load balancer whose backends disagree about the current service day (transit_core/NOTES.md 9);
// the balancer names the backend in `X-B-Srvr` and offers a sticky-session cookie in
// `Set-Cookie: AWSELB=...` (10 minute max-age, refreshed on every request that presents it).
struct ReplyInfo {
  std::string set_cookie;  // "AWSELB=<value>" with the attributes stripped, or empty
  std::string backend;     // X-B-Srvr, e.g. "api_main3", or empty
};

// Sticky BusSchedules backend: once pinned, get() sends the cookie with every URL containing
// "BusSchedules" so the balancer keeps routing to the backend that last answered for today.
// net_poller.cpp pins after a plausible answer and unpins after a wrong-day one, so a bad
// backend is abandoned on the next connection instead of being sticky for ten minutes.
void pinScheduleBackend(const std::string &cookie);
void unpinScheduleBackend();
const std::string &scheduleCookie();

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
// Retries happen only while no body bytes have reached `onData`; once any byte was
// delivered the call returns that attempt's status and the caller judges the body
// (SEPTA sometimes labels a valid body HTTP 501). Chunked and Content-Length bodies
// are both handled.
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
//
// `reply`, if non-null, receives the Set-Cookie and X-B-Srvr headers of the response that was
// kept (see ReplyInfo).
int get(const char *url, std::function<bool(const uint8_t *, size_t)> onData, uint32_t timeout_ms,
        bool tls_verify = true, ReplyInfo *reply = nullptr);

// Selects the transport for every get(): with `use_https` false, an https:// URL is fetched over
// plain http:// with a NetworkClient and no TLS at all. Default false: a TLS session needs ~40 KB
// of heap with two 16 KB contiguous buffers, which the classic ESP32 (no PSRAM) running LVGL,
// Wi-Fi, and a web server cannot spare (measured 2026-09-14: 51 KB free, 14 KB largest block at
// poll time). SEPTA serves every endpoint over http:// without redirecting. config.device.use_https.
void setUseHttps(bool use_https);
bool useHttps();

}  // namespace transit_app
