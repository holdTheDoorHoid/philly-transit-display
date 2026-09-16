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

#include "transit_core/source.h"  // transit::FetchResult / transit::HttpGetEx

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

// Performs one plain-HTTP GET of `url` (an https:// URL is rewritten to http://, see below).
// Retries up to 3 times total, with backoff of
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
// Returns the HTTP status code (e.g. 200) from the response that was
// ultimately kept (the first non-retried one, or the last attempt if all 3
// were retried), or a negative HTTPClient error code if no attempt ever got
// a response at all.
//
// `reply`, if non-null, receives the Set-Cookie and X-B-Srvr headers of the response that was
// kept (see ReplyInfo).
int get(const char *url, std::function<bool(const uint8_t *, size_t)> onData, uint32_t timeout_ms,
        ReplyInfo *reply = nullptr);

// Same request, same retry policy, but reporting what the transport actually knows about the body
// (review finding F13). A status code alone cannot answer "did we receive ALL of it?": a
// connection dropped halfway through the 150 KB TripUpdates feed still reports 200, and the
// decoder then sees a short-but-syntactically-fine feed - the stops the missing half would have
// filled come back empty and "successful" (transit_core/source.h). Fields, in the same terms
// transit::FetchResult documents:
//
//   status    the HTTP status of the kept response, or 0 when no response was obtained at all
//             (transit_core's HttpGet contract; get()'s negative HTTPClient error codes are
//             folded to 0 here, because transit_core tests `status == 0` for "unreachable").
//   complete  the body reached its end. For a Content-Length response that means the bytes
//             delivered equal HTTPClient::getSize(); for a chunked one it means writeToStream()
//             returned a non-negative count, which the core's HTTPClient.cpp only does after the
//             terminating zero-length chunk (any earlier drop returns CONNECTION_LOST /
//             READ_TIMEOUT / STREAM_WRITE). A body with neither framing (EOF-delimited) is
//             complete when the peer closed the connection, which IS its framing.
//   aborted   WE stopped it: `onData` returned false. Not a transport failure - the caller asked.
//   bytes     body bytes handed to `onData` (including the chunk a refusing onData rejected).
//
// A request that hit the absolute deadline below is `complete = false`, never a short success.
transit::FetchResult getEx(const char *url, std::function<bool(const uint8_t *, size_t)> onData,
                           uint32_t timeout_ms, ReplyInfo *reply = nullptr);

// Absolute per-request deadline (F13): 2x `timeout_ms`, capped at 30 s, armed when the attempt
// starts and enforced inside the body sink. HTTPClient's own timeout is per read, so a peer that
// trickles one byte before each window expires can hold the caller forever - and the caller here
// is the poller task, so "forever" means the display stops updating. When the deadline fires the
// transfer is cut and the result is reported incomplete.
uint32_t absoluteDeadlineMs(uint32_t timeout_ms);

// Plain HTTP unless the firmware is built with -DTRANSIT_HTTPS. The optional TLS mode shipped in
// v0.1.0-0.1.1 and was removed in v0.1.2; the shipping envs still fetch an https:// URL over
// http:// rather than refusing it (DESIGN.md SS2 "Transport"), because a TLS session on this SDK
// needs two 16,717 B contiguous record buffers plus ~20 KB around them - the precompiled mbedTLS
// cannot shrink them (DESIGN.md SS2.1 has the measurements) - and SEPTA, Open-Meteo and Bicycle
// Transit all serve plain http. The prototype below is the heap-gated answer to that.

#ifdef TRANSIT_HTTPS
// DESIGN.md SS2.1: how an https:// URL is fetched, config.device.transport.
//   Http            plain http:// for every URL (what the shipping envs do unconditionally).
//   HttpsPreferred  verified TLS when the heap gate says one session fits right now, else plain
//                   http:// for THAT fetch. The transport is chosen by the device's memory, never
//                   by the network: the gate runs BEFORE the connection, and a TLS attempt that
//                   then fails - certificate, handshake, timeout, or memory running out inside
//                   mbedTLS - is reported as unreachable and is NOT retried over http, so an
//                   on-path attacker cannot force a downgrade by breaking TLS or by sending a
//                   chain too big to parse (review finding F04). The next poll asks the gate again.
//   HttpsRequired   verified TLS or nothing: when the gate refuses, the fetch is reported as
//                   unreachable and the stop shows as such (DESIGN.md SS4.7 health).
enum class Transport : uint8_t { Http = 0, HttpsPreferred = 1, HttpsRequired = 2 };

// config_store.cpp pushes the active config's device.transport here from setActiveConfig(), so a
// PUT /api/config changes the policy for the very next fetch without the poller knowing.
void setTransportPolicy(Transport policy);
Transport transportPolicy();
const char *transportName(Transport policy);           // "http" | "https_preferred" | "https"
bool parseTransport(const char *name, Transport &out);  // the inverse; false for anything else

// What actually happened, for GET /api/state's "transport" block and the device test plan.
// Counters are per boot. `last` is the transport of the most recent fetch that asked for https://:
// "https" (verified TLS delivered a response), "http" (plain, by policy or by the heap gate),
// "https_failed" (TLS attempted, no response, not downgraded), "refused" (policy https, gate said no).
struct TransportStats {
  uint32_t https_ok = 0;
  uint32_t https_failed = 0;     // TLS attempted, no response: certificate, handshake, timeout
  uint32_t cert_failed = 0;      // subset of https_failed: chain did not verify against the pinned root, or no root pinned
  uint32_t http_by_policy = 0;   // policy http
  uint32_t http_by_heap = 0;     // policy https_preferred, heap gate refused, fetched over plain http
  uint32_t refused_by_heap = 0;  // policy https, heap gate refused, reported unreachable
  const char *last = "none";
  int last_tls_error = 0;        // mbedTLS code (negative) of the most recent TLS failure, 0 if none
  // Both are MALLOC_CAP_8BIT - what an allocation can actually get - not ESP.getFreeHeap(), which
  // on the classic ESP32 also counts ~34 KB of 32-bit-only IRAM heap no buffer can use (SS2.1).
  size_t gate_free = 0;          // byte-addressable free heap when the last gate decision was taken
  size_t gate_largest = 0;       // largest byte-addressable free block at that moment
  uint32_t last_https_ms = 0;    // connect + handshake + headers of the last successful TLS fetch
};
TransportStats transportStats();

// Bytes of free heap the gate requires before it lets a TLS session start (the measured session
// plus margin, http_fetch.cpp). Exposed so /api/state can show it next to the live numbers.
size_t tlsHeapNeed();
#endif  // TRANSIT_HTTPS

}  // namespace transit_app
