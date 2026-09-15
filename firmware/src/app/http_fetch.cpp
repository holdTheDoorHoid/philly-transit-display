#include "http_fetch.h"

#include <Arduino.h>

#include <cstring>
#include <string>
#include <HTTPClient.h>


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

uint32_t absoluteDeadlineMs(uint32_t timeout_ms) {
  uint64_t budget = (uint64_t)timeout_ms * 2;
  if (budget > kAbsoluteDeadlineCapMs) budget = kAbsoluteDeadlineCapMs;
  if (budget < 1000) budget = 1000;  // a deadline shorter than a second is a bug, not a policy
  return (uint32_t)budget;
}

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
  // HTTPS is DEFERRED, not unimplemented (http_fetch.h, DESIGN.md SS2 "Transport"): an https:// URL
  // is deliberately fetched over http:// so a config or a hardcoded SEPTA URL keeps working.
  if (strncmp(url, "https://", 8) == 0) {
    plain_url = std::string("http://") + (url + 8);
    url = plain_url.c_str();
  }
  const uint32_t budget_ms = absoluteDeadlineMs(timeout_ms);
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) {
      uint32_t backoff_ms = kBackoffBaseMs << (attempt - 1);
      log_i("http_fetch: attempt %d/%d for %s in %ums (previous status %d)", attempt + 1, kMaxAttempts, url, backoff_ms, last_status);
      delay(backoff_ms);
    }

    NetworkClient plain_client;
    NetworkClient *client = &plain_client;

    HTTPClient http;
    http.setConnectTimeout((int32_t)timeout_ms);
    http.setTimeout((uint16_t)timeout_ms);
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
    if (reply != nullptr) {
      reply->set_cookie = cookieNameValue(http.header("Set-Cookie"));
      reply->backend = std::string(http.header("X-B-Srvr").c_str());
    }
    size_t presented = 0;
    bool complete = false, refused = false, timed_out = false;
    if (status > 0) {
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
