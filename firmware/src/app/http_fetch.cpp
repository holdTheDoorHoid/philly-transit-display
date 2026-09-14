#include "http_fetch.h"

#include <Arduino.h>

#include <cstring>
#include <string>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "ca_bundle.h"

namespace transit_app {

namespace {

constexpr int kMaxAttempts = 3;
constexpr uint32_t kBackoffBaseMs = 500;  // 500, 1000, 2000

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
class CallbackStream : public Stream {
 public:
  explicit CallbackStream(const std::function<bool(const uint8_t *, size_t)> &onData) : onData_(onData) {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buffer, size_t size) override {
    delivered_ += size;
    if (aborted_) return 0;
    if (onData_ && !onData_(buffer, size)) {
      aborted_ = true;
      return 0;  // HTTPClient treats a short write as an error and stops
    }
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t delivered() const { return delivered_; }
  bool aborted() const { return aborted_; }

 private:
  const std::function<bool(const uint8_t *, size_t)> &onData_;
  size_t delivered_ = 0;
  bool aborted_ = false;
};

}  // namespace

bool g_use_https = false;

void setUseHttps(bool use_https) {
  g_use_https = use_https;
}

bool useHttps() {
  return g_use_https;
}

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

int get(const char *url, std::function<bool(const uint8_t *, size_t)> onData, uint32_t timeout_ms,
        bool tls_verify, ReplyInfo *reply) {
  int last_status = -1;

  std::string plain_url;
  if (!g_use_https && strncmp(url, "https://", 8) == 0) {
    plain_url = std::string("http://") + (url + 8);
    url = plain_url.c_str();
  }
  const bool https = strncmp(url, "https://", 8) == 0;

  if (https && !tls_verify) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      log_w("http_fetch: tls_verify is OFF (config.device.tls_verify=false) - certificates are NOT checked");
    }
  }

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) {
      uint32_t backoff_ms = kBackoffBaseMs << (attempt - 1);
      log_i("http_fetch: attempt %d/%d for %s in %ums (previous status %d)", attempt + 1, kMaxAttempts, url, backoff_ms, last_status);
      delay(backoff_ms);
    }

    // Only one of these is used per attempt; both live on this task's stack (small objects, the
    // TLS context itself is heap-allocated by NetworkClientSecure on connect).
    NetworkClientSecure secure_client;
    NetworkClient plain_client;
    NetworkClient *client = &plain_client;
    if (https) {
      if (tls_verify) {
        secure_client.setCACertBundle(kSeptaCaBundle, kSeptaCaBundleLen);
      } else {
        secure_client.setInsecure();
      }
      client = &secure_client;
    }

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

    int status = http.GET();
    if (reply != nullptr) {
      reply->set_cookie = cookieNameValue(http.header("Set-Cookie"));
      reply->backend = std::string(http.header("X-B-Srvr").c_str());
    }
    size_t delivered = 0;
    if (status > 0) {
      CallbackStream sink(onData);
      http.writeToStream(&sink);
      delivered = sink.delivered();
    }
    http.end();
    last_status = status;

    // Retry only when the consumer has not seen any body bytes: a streaming consumer cannot
    // un-see a partial or mislabeled response (SEPTA returns HTTP 501 with a perfectly valid
    // body, see transit_core/NOTES.md), so once bytes flowed the caller judges the body.
    if (!isRetryableStatus(status) || delivered > 0) {
      return status;
    }
  }

  return last_status;
}

}  // namespace transit_app
