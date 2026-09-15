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

// Plain HTTP only. The optional TLS mode shipped in v0.1.0-0.1.1 and was removed in v0.1.2; HTTPS
// is DEFERRED by the owner's decision (DESIGN.md SS2 "Transport"), not merely unimplemented, so an
// https:// URL here is deliberately fetched over http:// rather than refused. The reasons stand:
// TLS cost ~100 KB of flash, a session needs ~40 KB of heap with two 16 KB contiguous buffers that
// the classic ESP32 (no PSRAM) running LVGL, Wi-Fi and a web server cannot spare, and SEPTA,
// Open-Meteo and Bicycle Transit all serve plain http. Revisit only with DESIGN.md SS2.

}  // namespace transit_app
