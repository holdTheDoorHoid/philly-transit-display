#include "http_fetch.h"

#include <Arduino.h>

#include <cstring>
#include <algorithm>
#include <string>
#include <HTTPClient.h>

#ifdef TRANSIT_HTTPS
#include <NetworkClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/bignum.h>
#include <mbedtls/cipher.h>
#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>

#include <memory>
#include <new>

#include "tls_roots.h"
#endif


namespace transit_app {

namespace {

constexpr int kMaxAttempts = 3;
constexpr uint32_t kBackoffBaseMs = 500;  // 500, 1000, 2000
// F13: no single request may exceed this, whatever the caller's per-read timeout is.
constexpr uint32_t kAbsoluteDeadlineCapMs = 30000;

bool isRetryableStatus(int status) {
  if (status < 0) {
    return true;  // connection/handshake-level failure
  }
  switch (status) {
    case 400:
    case 501:
    case 502:
    case 503:
      return true;
    default:
      return false;
  }
}

// Adapts HTTPClient::writeToStream() (which understands both Content-Length and chunked
// transfer-encoding) to the onData callback. Never buffers the body.
//
// Two things beyond plumbing (F13):
//   * It separates "the consumer refused" from "the transport failed". Both look identical to
//     HTTPClient - a short write, which it reports as HTTPC_ERROR_STREAM_WRITE after one retry -
//     so the sink has to remember which it was, or every size-capped fetch reads as a broken one.
//   * It enforces the absolute deadline. HTTPClient's timeout is per read; a peer trickling a
//     byte per window never trips it, and this runs on the poller task.
class CallbackStream : public Stream {
 public:
  CallbackStream(const std::function<bool(const uint8_t *, size_t)> &onData, uint32_t deadline_ms)
      : onData_(onData), deadline_ms_(deadline_ms) {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    // HTTPClient retries a short write once with the same bytes (writeToStreamDataBlock in the
    // core's HTTPClient.cpp); returning 0 again without re-presenting them keeps `presented_`
    // honest and keeps a refusing consumer from seeing the same chunk twice.
    if (stopped_) return 0;
    if ((int32_t)(millis() - deadline_ms_) >= 0) {
      stopped_ = true;
      timed_out_ = true;
      return 0;
    }
    presented_ += size;
    if (onData_ && !onData_(buffer, size)) {
      stopped_ = true;
      refused_ = true;
      return 0;  // HTTPClient treats a short write as an error and stops
    }
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  // Bytes handed to onData, counting the chunk a refusing consumer rejected: it saw them, so a
  // retry of this attempt would re-deliver data the consumer has already acted on.
  size_t presented() const { return presented_; }
  bool refused() const { return refused_; }
  bool timedOut() const { return timed_out_; }

 private:
  const std::function<bool(const uint8_t *, size_t)> &onData_;
  uint32_t deadline_ms_ = 0;
  size_t presented_ = 0;
  bool stopped_ = false;
  bool refused_ = false;
  bool timed_out_ = false;
};

}  // namespace

namespace {
std::string g_sched_cookie;

std::string cookieNameValue(const String &set_cookie) {
  int semi = set_cookie.indexOf(';');
  String nv = semi >= 0 ? set_cookie.substring(0, semi) : set_cookie;
  nv.trim();
  return std::string(nv.c_str());
}
}  // namespace

void pinScheduleBackend(const std::string &cookie) {
  if (!cookie.empty()) g_sched_cookie = cookie;
}
void unpinScheduleBackend() {
  g_sched_cookie.clear();
}
const std::string &scheduleCookie() {
  return g_sched_cookie;
}

// A single blocking stream read must stay under the 5 s task watchdog (see get()).
constexpr uint32_t kStreamReadTimeoutMs = 4000;

uint32_t absoluteDeadlineMs(uint32_t timeout_ms) {
  uint64_t budget = (uint64_t)timeout_ms * 2;
  if (budget > kAbsoluteDeadlineCapMs) budget = kAbsoluteDeadlineCapMs;
  if (budget < 1000) budget = 1000;  // a deadline shorter than a second is a bug, not a policy
  return (uint32_t)budget;
}

#ifdef TRANSIT_HTTPS
// ---------------------------------------------------------------------------------------------
// TLS on a board with ~75 KB of heap (DESIGN.md SS2.1). Every number below was read off this SDK
// (Arduino-ESP32 3.2.1 / ESP-IDF 5.4.2, mbedTLS 3.6.3, sdkconfig CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN
// 16384, CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH unset) with a sizeof probe against its headers,
// not guessed. The precompiled libmbedtls cannot be told to use smaller buffers: max_fragment_length
// negotiation is compiled in, but the code that would shrink the buffers after negotiating it is
// not, so an MFL of 4096 only changes what the server sends per record, not what we allocate.
// ---------------------------------------------------------------------------------------------
namespace {

// mbedtls_ssl_setup() callocs the input and output record buffers back to back, each
// MBEDTLS_SSL_IN_BUFFER_LEN = 13 (header) + 16384 (content) + 320 (IV, MAC, CBC padding) bytes.
// They have to be contiguous blocks, and they live for the whole session - through the body.
constexpr size_t kTlsRecordBuffer = 16717;
// Contexts NetworkClientSecure `new`s per connection (2.1 KB: ssl 552 B, config 196, entropy 420,
// two x509_crt 408 each, ctr_drbg 76), the kept peer chain (the SDK sets
// MBEDTLS_SSL_KEEP_PEER_CERTIFICATE, so the three parsed certificates SEPTA sends, ~6 KB, stay for
// the session), the negotiated transform and the socket.
constexpr size_t kTlsSessionOther = 10 * 1024;
// Freed again after the handshake: handshake params (944 B), the parsed pinned root (~2 KB, 4096-bit
// ISRG Root X1 ~2.5 KB), ECDHE P-256 and RSA-2048 bignum scratch, the SHA contexts.
constexpr size_t kTlsHandshakeScratch = 12 * 1024;
// What the rest of the poll cycle and the web server (its heavy handlers refuse below an 8 KB
// largest block, web_server.cpp) need to keep working while the session is up.
constexpr size_t kFetchMargin = 12 * 1024;
#ifdef TRANSIT_HTTPS_GATE_TEST
// Test hook (DESIGN.md SS2.1 device test, never in a shipped image): let the gate pass whenever the
// two record buffers alone fit, so the real mbedTLS path runs on a board that cannot actually afford
// it and the fail-closed handling of an allocation failure mid-handshake can be watched.
//   PLATFORMIO_BUILD_FLAGS=-DTRANSIT_HTTPS_GATE_TEST pio run -e <env>
constexpr size_t kTlsHeapNeed = 2 * kTlsRecordBuffer;
constexpr size_t kTlsScratchLargestBlock = 0;
#else
constexpr size_t kTlsHeapNeed = 2 * kTlsRecordBuffer + kTlsSessionOther + kTlsHandshakeScratch + kFetchMargin;
// The biggest single allocation the handshake makes is a parsed 4096-bit certificate; require
// that much contiguous space AFTER the two record buffers are carved out.
constexpr size_t kTlsScratchLargestBlock = 6 * 1024;
#endif
// mbedTLS drives a non-blocking socket in ssl_client.cpp's handshake loop (vTaskDelay(2) between
// steps, so IDLE0 is fed); this caps the whole exchange. RSA-2048 verification x3 plus ECDHE
// P-256 on this chip is well under 2 s; 8 s leaves room for a slow AWS front end.
constexpr uint32_t kTlsHandshakeTimeoutS = 8;

}  // namespace

// mbedTLS's error.c - the string table behind mbedtls_strerror() - is ~16 KB of flash on this
// SDK (link map, DESIGN.md SS2.1), and the core's ssl_client.cpp calls it on its error path, so it
// would come along for the sake of one log line. An object file's definition beats an archive
// member's at link time, so defining the symbol here keeps error.c out of the image entirely. The
// log carries the numeric code; mbedtls/error.h (or `python -m mbedtls_error` in mbedTLS's tree)
// names it for whoever reads the log.
extern "C" void mbedtls_strerror(int ret, char *buf, size_t buflen) {
  snprintf(buf, buflen, "mbedTLS -0x%04X", (unsigned)(-ret));
}

// Same trick for the TLS 1.2 *server*: the SDK's libmbedtls is built TLS_SERVER_AND_CLIENT and
// ssl_tls.c's handshake stepper names the server-side stepper, so a client-only image would still
// carry ssl_tls12_server.c (7.9 KB; `nm` shows this is the only symbol that pulls it in).
// NetworkClientSecure configures MBEDTLS_SSL_IS_CLIENT and nothing else, so this stub can never
// be reached; if a future SDK ever needed another symbol from that object the link would fail
// loudly with a duplicate definition, which is the failure mode we want.
extern "C" int mbedtls_ssl_handshake_server_step(mbedtls_ssl_context *) {
  return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

namespace {

Transport g_policy = Transport::Http;  // until setActiveConfig() pushes the config's device.transport
TransportStats g_stats;
// Written on the poller task, read on the async web server's task for /api/state.
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;

template <typename F>
void updateStats(F &&f) {
  portENTER_CRITICAL(&g_stats_mux);
  f(g_stats);
  portEXIT_CRITICAL(&g_stats_mux);
}

std::string hostFromUrl(const char *url) {
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  const char *e = p;
  while (*e && *e != '/' && *e != ':' && *e != '?') ++e;
  return std::string(p, e);
}

// The gate. free/largest alone cannot answer "will two 16.7 KB blocks fit?", only holding one
// while asking for the other can, and the allocator will hand mbedtls_ssl_setup() the same two
// holes a few milliseconds later. Costs two mallocs and two frees.
bool tlsAffordable(size_t &free_out, size_t &largest_out) {
  free_out = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  largest_out = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (free_out < kTlsHeapNeed || largest_out < kTlsRecordBuffer) return false;
  void *a = heap_caps_malloc(kTlsRecordBuffer, MALLOC_CAP_8BIT);
  void *b = a ? heap_caps_malloc(kTlsRecordBuffer, MALLOC_CAP_8BIT) : nullptr;
  const bool ok = b != nullptr && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= kTlsScratchLargestBlock;
  heap_caps_free(b);
  heap_caps_free(a);
  return ok;
}

// mbedTLS codes are -(high | low): the SSL/X509/PK/ECP/MD/CIPHER modules use bits 7-14, MPI the
// low 7 bits, and a low-level failure is added to whichever high-level one it surfaced through.
int tlsHighCode(int code) { return -((-code) & 0x7F80); }
int tlsLowCode(int code) { return -((-code) & 0x007F); }

// "The chain does not verify against the pinned root" (hostname mismatch is reported the same
// way by mbedTLS: MBEDTLS_X509_BADCERT_CN_MISMATCH folds into CERT_VERIFY_FAILED).
bool isCertFailure(int code) {
  const int hi = tlsHighCode(code);
  return hi == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || hi == MBEDTLS_ERR_SSL_BAD_CERTIFICATE;
}

// "The heap ran out under TLS" - the gate said yes and another task took the space in between, OR
// the peer sent something (a certificate chain, a handshake message) too big to parse in what was
// left. The two cannot be told apart from the code, which is why doGet() only logs this class.
bool isAllocFailure(int code) {
  const int hi = tlsHighCode(code);
  return hi == MBEDTLS_ERR_SSL_ALLOC_FAILED || hi == MBEDTLS_ERR_X509_ALLOC_FAILED ||
         hi == MBEDTLS_ERR_PK_ALLOC_FAILED || hi == MBEDTLS_ERR_ECP_ALLOC_FAILED ||
         hi == MBEDTLS_ERR_MD_ALLOC_FAILED || hi == MBEDTLS_ERR_CIPHER_ALLOC_FAILED ||
         tlsLowCode(code) == MBEDTLS_ERR_MPI_ALLOC_FAILED;
}

}  // namespace

void setTransportPolicy(Transport policy) {
  g_policy = policy;
}
Transport transportPolicy() {
  return g_policy;
}
const char *transportName(Transport policy) {
  switch (policy) {
    case Transport::Http: return "http";
    case Transport::HttpsRequired: return "https";
    case Transport::HttpsPreferred:
    default: return "https_preferred";
  }
}
bool parseTransport(const char *name, Transport &out) {
  if (name == nullptr) return false;
  if (strcmp(name, "http") == 0) { out = Transport::Http; return true; }
  if (strcmp(name, "https_preferred") == 0) { out = Transport::HttpsPreferred; return true; }
  if (strcmp(name, "https") == 0) { out = Transport::HttpsRequired; return true; }
  return false;
}
TransportStats transportStats() {
  TransportStats copy;
  portENTER_CRITICAL(&g_stats_mux);
  copy = g_stats;
  portEXIT_CRITICAL(&g_stats_mux);
  return copy;
}
size_t tlsHeapNeed() {
  return kTlsHeapNeed;
}
#endif  // TRANSIT_HTTPS

namespace {

// One implementation behind both get() and getEx(). `raw_status`, when non-null, receives the
// value get() has always returned (a negative HTTPClient error code when no response was
// obtained); FetchResult::status folds those to 0 because that is transit_core's "unreachable"
// signal (source.h).
transit::FetchResult doGet(const char *url, std::function<bool(const uint8_t *, size_t)> onData,
                           uint32_t timeout_ms, ReplyInfo *reply, int *raw_status) {
  transit::FetchResult result;
  int last_status = -1;

  std::string plain_url;
  const bool asked_https = strncmp(url, "https://", 8) == 0;
  auto rewriteToPlain = [&]() {
    plain_url = std::string("http://") + (url + 8);
    url = plain_url.c_str();
  };
#ifdef TRANSIT_HTTPS
  // DESIGN.md SS2.1: the heap, not the network, decides whether this fetch gets TLS.
  const Transport policy = g_policy;
  bool use_tls = false;
  const char *root_pem = nullptr;
  if (asked_https && policy != Transport::Http) {
    size_t free_b = 0, largest = 0;
    use_tls = tlsAffordable(free_b, largest);
    updateStats([&](TransportStats &s) {
      s.gate_free = free_b;
      s.gate_largest = largest;
    });
    if (!use_tls) {
      if (policy == Transport::HttpsRequired) {
        updateStats([](TransportStats &s) { s.refused_by_heap++; s.last = "refused"; });
        Serial.printf("[https] refused %s: heap free=%u largest=%u, need %u (policy https)\n", url,
                      (unsigned)free_b, (unsigned)largest, (unsigned)kTlsHeapNeed);
        if (raw_status != nullptr) *raw_status = HTTPC_ERROR_CONNECTION_REFUSED;
        return result;  // status 0: transit_core's "could not be made at all"
      }
      updateStats([](TransportStats &s) { s.http_by_heap++; s.last = "http"; });
      Serial.printf("[https] plain http for %s: heap free=%u largest=%u, need %u (policy https_preferred)\n",
                    url, (unsigned)free_b, (unsigned)largest, (unsigned)kTlsHeapNeed);
    } else {
      const std::string host = hostFromUrl(url);
      root_pem = pinnedRootsForHost(host.c_str());
      if (root_pem == nullptr) {
        // A host nobody pinned is a configuration change, not a memory problem: fail closed
        // (tls_roots.h), the same way an unverifiable chain does.
        updateStats([](TransportStats &s) { s.cert_failed++; s.https_failed++; s.last = "https_failed"; });
        Serial.printf("[https] no pinned root for host %s; refusing (tls_roots.h)\n", host.c_str());
        if (raw_status != nullptr) *raw_status = HTTPC_ERROR_CONNECTION_REFUSED;
        return result;
      }
    }
  } else if (asked_https) {
    updateStats([](TransportStats &s) { s.http_by_policy++; s.last = "http"; });
  }
  if (asked_https && !use_tls) rewriteToPlain();
#else
  // HTTPS is DEFERRED in the shipping envs, not unimplemented (http_fetch.h, DESIGN.md SS2
  // "Transport"): an https:// URL is deliberately fetched over http:// so a config or a hardcoded
  // SEPTA URL keeps working.
  if (asked_https) rewriteToPlain();
#endif
  const uint32_t budget_ms = absoluteDeadlineMs(timeout_ms);
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) {
      uint32_t backoff_ms = kBackoffBaseMs << (attempt - 1);
      log_i("http_fetch: attempt %d/%d for %s in %ums (previous status %d)", attempt + 1, kMaxAttempts, url, backoff_ms, last_status);
      delay(backoff_ms);
    }

    NetworkClient plain_client;
    NetworkClient *client = &plain_client;
#ifdef TRANSIT_HTTPS
    // Built only for a TLS attempt: the constructor alone `new`s the 2.1 KB of mbedTLS contexts,
    // and the record buffers follow inside connect(). Freed when this attempt's scope ends, so a
    // retry backoff or the idle gap never holds a TLS session (DESIGN.md SS4.7).
    std::unique_ptr<NetworkClientSecure> secure;
    if (use_tls) {
      secure.reset(new (std::nothrow) NetworkClientSecure());
      if (!secure) {
        last_status = HTTPC_ERROR_CONNECTION_REFUSED;
        continue;  // the pollOnce() bad_alloc guard is the backstop; here we just retry
      }
      secure->setCACert(root_pem);  // pointer only; parsed inside connect(), freed after the handshake
      secure->setHandshakeTimeout(kTlsHandshakeTimeoutS);
      client = secure.get();
    }
    const uint32_t started_ms = millis();
#endif

    HTTPClient http;
    // Cap the per-read/connect timeout well under this SDK's 5 s task watchdog. HTTPClient waits
    // for the response line and each header in Stream::timedRead() - a busy loop that yields only
    // to same-or-higher-priority tasks, so at the poller's priority 1 it does NOT let IDLE0 (the
    // task the watchdog checks) run. A single read that blocked for the full fetch timeout would
    // therefore panic the watchdog. Bounding one read to kStreamReadTimeoutMs keeps every busy-wait
    // under 5 s; a streaming body still flows because each read returns as soon as bytes arrive,
    // and the overall fetch budget is preserved by absoluteDeadlineMs() and the retry loop.
    // The same cap reaches a TLS socket: NetworkClientSecure passes it to select() for the TCP
    // connect and to SO_RCVTIMEO, and the handshake loop has its own bound (kTlsHandshakeTimeoutS).
    const uint16_t read_to = (uint16_t)std::min<uint32_t>(timeout_ms, kStreamReadTimeoutMs);
    http.setConnectTimeout((int32_t)read_to);
    http.setTimeout(read_to);
    http.setReuse(false);  // DESIGN.md SS4.7: never hold a socket across the idle gap

    if (!http.begin(*client, url)) {
      log_e("http_fetch: begin() failed for %s (bad URL?)", url);
      last_status = -1;
      continue;
    }
    static const char *kCollect[] = {"Set-Cookie", "X-B-Srvr"};
    http.collectHeaders(kCollect, 2);
    if (!g_sched_cookie.empty() && strstr(url, "BusSchedules") != nullptr) {
      http.addHeader("Cookie", g_sched_cookie.c_str());
    }

    // Armed before the request goes out, so connect + headers + body share one absolute budget.
    const uint32_t deadline_ms = millis() + budget_ms;
    int status = http.GET();
#ifdef TRANSIT_HTTPS
    if (use_tls && status <= 0) {
      // No response over TLS. Classify for the log and the counters; nothing classified here
      // ever turns into plain http (see http_fetch.h and the note below on allocation failures).
      char ebuf[80] = {0};
      const int code = secure->lastError(ebuf, sizeof ebuf);
      Serial.printf("[https] %s attempt %d/%d: no response over TLS after %u ms, code -0x%04x %s (heap free=%u largest=%u)\n",
                    url, attempt + 1, kMaxAttempts, (unsigned)(millis() - started_ms), (unsigned)(-code), ebuf,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      http.end();
      if (isCertFailure(code)) {
        updateStats([code](TransportStats &s) { s.cert_failed++; s.https_failed++; s.last = "https_failed"; s.last_tls_error = code; });
        if (raw_status != nullptr) *raw_status = HTTPC_ERROR_CONNECTION_REFUSED;
        return result;  // will not fix itself in 2 s of backoff, and must never become plain http
      }
      // An allocation failure here is NOT a reason to fall back to plain http, even under
      // https_preferred, although the gate's answer would have been "no" a moment earlier: the
      // same error codes come out of mbedTLS while it parses the PEER's certificate chain
      // (MBEDTLS_ERR_X509_ALLOC_FAILED for an oversized chain, SSL_ALLOC_FAILED for a handshake
      // message it cannot buffer), so a fetch that downgraded on them could be downgraded by
      // whoever answers the connection. Once the gate has chosen TLS for a fetch, the fetch is
      // TLS or nothing (review F04); it is logged apart so a "heap moved under us" shows up as
      // that in the serial log, and the normal retry policy applies - by the second attempt the
      // web server has usually finished whatever took the space.
      if (isAllocFailure(code)) {
        Serial.printf("[https] %s: out of memory inside TLS after the gate passed (free=%u largest=%u); staying on TLS\n",
                      url, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      }
      updateStats([code](TransportStats &s) { s.last_tls_error = code; });
      last_status = status;
      continue;  // handshake timeout / TCP / memory failure: the normal retry policy, still over TLS
    }
#endif
    if (reply != nullptr) {
      reply->set_cookie = cookieNameValue(http.header("Set-Cookie"));
      reply->backend = std::string(http.header("X-B-Srvr").c_str());
    }
    size_t presented = 0;
    bool complete = false, refused = false, timed_out = false;
    if (status > 0) {
#ifdef TRANSIT_HTTPS
      if (use_tls) {
        const uint32_t took = millis() - started_ms;
        updateStats([took](TransportStats &s) { s.https_ok++; s.last = "https"; s.last_https_ms = took; });
        // The one line the device test plan reads (DESIGN.md SS2.1): heap with the session UP.
        Serial.printf("[https] %s: %d over TLS, headers in %u ms; heap free=%u largest=%u min_free=%u\n", url, status,
                      (unsigned)took, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
      }
#endif
      // Captured BEFORE writeToStream: for a chunked response HTTPClient back-fills _size with
      // the total it just read, so asking afterwards would compare a number with itself.
      const int content_length = http.getSize();
      CallbackStream sink(onData, deadline_ms);
      int written = http.writeToStream(&sink);
      presented = sink.presented();
      refused = sink.refused();
      timed_out = sink.timedOut();
      if (!refused && !timed_out && written >= 0) {
        // written >= 0 means the framing terminated cleanly: writeToStreamDataBlock() itself
        // returns HTTPC_ERROR_STREAM_WRITE on a Content-Length mismatch, and the chunked loop
        // only breaks out (rather than returning an error) on the zero-length chunk. The
        // length check is belt and braces on top of that.
        complete = content_length < 0 || presented == (size_t)content_length;
      }
    }
    http.end();
    last_status = status;

    // Retry only when the consumer has not seen any body bytes: a streaming consumer cannot
    // un-see a partial or mislabeled response (SEPTA returns HTTP 501 with a perfectly valid
    // body, see transit_core/NOTES.md), so once bytes flowed the caller judges the body.
    if (!isRetryableStatus(status) || presented > 0) {
      if (timed_out) {
        log_w("http_fetch: absolute deadline (%u ms) hit for %s after %u bytes; body incomplete",
              (unsigned)budget_ms, url, (unsigned)presented);
      }
      if (raw_status != nullptr) *raw_status = status;
      result.status = status > 0 ? status : 0;
      result.complete = complete;
      result.aborted = refused;
      result.bytes = presented;
      return result;
    }
  }

#ifdef TRANSIT_HTTPS
  if (use_tls) {
    // Every TLS attempt failed at the transport level. Reported as unreachable; not downgraded.
    updateStats([](TransportStats &s) { s.https_failed++; s.last = "https_failed"; });
  }
#endif
  if (raw_status != nullptr) *raw_status = last_status;
  result.status = last_status > 0 ? last_status : 0;
  return result;
}

}  // namespace

int get(const char *url, std::function<bool(const uint8_t *, size_t)> onData, uint32_t timeout_ms,
        ReplyInfo *reply) {
  int raw = -1;
  doGet(url, std::move(onData), timeout_ms, reply, &raw);
  return raw;
}

transit::FetchResult getEx(const char *url, std::function<bool(const uint8_t *, size_t)> onData,
                           uint32_t timeout_ms, ReplyInfo *reply) {
  return doGet(url, std::move(onData), timeout_ms, reply, nullptr);
}

}  // namespace transit_app
