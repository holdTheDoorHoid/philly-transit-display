#include "http_fetch.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "ca_bundle.h"

namespace transit_app {

namespace {

constexpr int kMaxAttempts = 3;
constexpr uint32_t kBackoffBaseMs = 500;  // 500, 1000, 2000
constexpr size_t kChunkSize = 512;

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

// Reads the response body in kChunkSize pieces, handing each to `onData`.
// Relies on Content-Length (http.getSize()); SEPTA's endpoints (DESIGN.md
// SS4) are all small JSON/protobuf responses served with an explicit
// Content-Length, so this does not implement chunked transfer-decoding.
void streamBody(HTTPClient &http, NetworkClient *stream, uint32_t timeout_ms, const std::function<bool(const uint8_t *, size_t)> &onData) {
  int remaining = http.getSize();  // -1 if unknown (would need chunked decoding)
  uint8_t buf[kChunkSize];
  uint32_t deadline = millis() + timeout_ms;

  while (http.connected() && (remaining > 0 || remaining == -1)) {
    if ((int32_t)(millis() - deadline) > 0) {
      log_w("http_fetch: body read timed out with %d byte(s) still expected", remaining);
      break;
    }
    size_t avail = stream->available();
    if (avail == 0) {
      if (!stream->connected()) {
        break;
      }
      delay(1);
      continue;
    }
    size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
    if (remaining > 0 && (size_t)remaining < want) {
      want = (size_t)remaining;
    }
    int n = stream->readBytes(buf, want);
    if (n <= 0) {
      break;
    }
    if (remaining > 0) {
      remaining -= n;
    }
    if (onData && !onData(buf, (size_t)n)) {
      break;  // consumer asked to stop early
    }
  }
}

}  // namespace

int get(const char *url, std::function<bool(const uint8_t *, size_t)> onData, uint32_t timeout_ms) {
  int last_status = -1;

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) {
      uint32_t backoff_ms = kBackoffBaseMs << (attempt - 1);
      log_i("http_fetch: attempt %d/%d for %s in %ums (previous status %d)", attempt + 1, kMaxAttempts, url, backoff_ms, last_status);
      delay(backoff_ms);
    }

    NetworkClientSecure client;
    client.setCACertBundle(kSeptaCaBundle, kSeptaCaBundleLen);

    HTTPClient http;
    http.setConnectTimeout((int32_t)timeout_ms);
    http.setTimeout((uint16_t)timeout_ms);
    http.setReuse(false);  // DESIGN.md SS4.7: never hold a TLS socket across the idle gap

    if (!http.begin(client, url)) {
      log_e("http_fetch: begin() failed for %s (bad URL?)", url);
      last_status = -1;
      continue;
    }

    int status = http.GET();
    if (status > 0) {
      streamBody(http, http.getStreamPtr(), timeout_ms, onData);
    }
    http.end();
    last_status = status;

    if (!isRetryableStatus(status)) {
      return status;
    }
  }

  return last_status;
}

}  // namespace transit_app
