#include "proxy_worker.h"

#include <Arduino.h>

#include <new>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include "config_store.h"
#include "http_fetch.h"
#include "sd_logger.h"
#include "transit_core/septa_source.h"
#include "transit_stats/aggregate.h"
#include "transit_stats/log_window.h"
#include "transit_stats/overview.h"

namespace transit_app {

namespace {

constexpr uint32_t kFetchTimeoutMs = 15000;
constexpr UBaseType_t kQueueLen = 2;  // each paused request holds a TCP pcb and buffers
// StatsAggregator alone can be just under 8KB (its own size assertion in test_stats), so this
// must comfortably clear that plus the rest of runStatsJob()'s frame (JsonDocument, String,
// lambdas) even with it heap-allocated rather than a local - a smaller stack here silently
// corrupts memory instead of failing loudly, so err generous.
constexpr int kDefaultStatsDays = 30;
constexpr int kMaxStatsDays = 365;

enum class ProxyKind { Stops, Schedule, Stats, Overview };

// Shared between the web-server task (which creates this on a queued request) and this file's
// worker task (which reads it after a possibly-long network fetch or SD scan): a request whose
// client has gone away must never be touched again (its AsyncWebServerRequest may already be
// freed), so onDisconnect() flips this instead of deleting anything out from under the worker.
struct ProxyJob {
  ProxyKind kind;
  std::string param;  // route (Stops), stop_id (Schedule), or stop key (Stats); unused for Overview
  int days = kDefaultStatsDays;  // Stats and Overview
  // Weak pointer from AsyncWebServerRequest::pause(): the server keeps the request alive until we
  // send (or the client aborts, in which case lock() returns null and we drop the job).
  AsyncWebServerRequestPtr request;
};

QueueHandle_t g_queue = nullptr;

std::string urlEncodeSpaces(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == ' ') {
      out += "%20";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string septaStopsUrl(const std::string &route) {
  return "https://www3.septa.org/api/Stops/index.php?req1=" + urlEncodeSpaces(route);
}

bool requestStillAlive(const ProxyJob &job) {
  auto r = job.request.lock();
  return r && r->client() != nullptr && r->client()->connected();
}

// Locks the paused request for sending; null if the client went away.
std::shared_ptr<AsyncWebServerRequest> lockRequest(const ProxyJob &job) {
  auto r = job.request.lock();
  if (!r || r->client() == nullptr || !r->client()->connected()) return nullptr;
  return r;
}

void sendError(AsyncWebServerRequest *request, int code, const std::string &message) {
  request->send(code, "application/json", (std::string("{\"error\":\"") + message + "\"}").c_str());
}

// Proxied SEPTA bodies are streamed into a LittleFS temp file and served from there, never
// buffered in RAM: a 12 KB Stops list needs a 16 KB contiguous block while a std::vector grows,
// and after Wi-Fi + web server the largest free block is ~15 KB (boot loop observed 2026-09-14).
constexpr size_t kProxyFileCap = 64 * 1024;
const char *kProxyFiles[2] = {"/proxy0.json", "/proxy1.json"};

// ---- temp-file leases (F11) -------------------------------------------------------------------
//
// The two files used to be handed out by a plain alternating index, on the theory that "a
// response still being streamed is not overwritten". That is only true for exactly two requests
// in flight and no more: a third job wraps back to file 0 and rewrites it UNDER an
// AsyncWebServer response that is still reading it, so a setup-wizard client gets half a stop
// list spliced onto half a schedule, with a 200 and valid-looking JSON framing. Nothing in the
// old code could detect it; the browser just showed the wrong stops.
//
// So a file is LEASED for the whole life of the response that serves it, and released by the
// request's disconnect callback - which ESPAsyncWebServer fires on both endings (WebRequest.cpp:
// when the response finishes, _onAck closes the client, and closing is what triggers
// _onDisconnect; an aborted client reaches the same callback). With both leased, a new job is
// refused with 503 rather than corrupting one.
constexpr uint32_t kLeaseMaxMs = 60 * 1000;

struct ProxyFileLease {
  std::atomic<bool> held{false};
  uint32_t taken_ms = 0;
};
ProxyFileLease g_proxy_leases[2];

// Acquire runs only on the poller task (runQueuedProxyJob), release on the AsyncTCP task - hence
// the atomics. Returns the file index, or -1 when both are busy.
int acquireProxyFile() {
  for (int i = 0; i < 2; ++i) {
    bool expected = false;
    if (g_proxy_leases[i].held.compare_exchange_strong(expected, true)) {
      g_proxy_leases[i].taken_ms = millis();
      return i;
    }
  }
  // Last resort: a lease this old cannot belong to a live response - a 64 KB file served off
  // LittleFS over the LAN is a second or two - so it is one whose disconnect callback never ran
  // (the client vanished in the window between locking the request and registering the callback).
  // Without this, one such miss would wedge the setup wizard until the next reboot.
  for (int i = 0; i < 2; ++i) {
    if ((millis() - g_proxy_leases[i].taken_ms) > kLeaseMaxMs) {
      Serial.printf("[proxy] reclaiming abandoned lease on %s after %u ms\n", kProxyFiles[i],
                    (unsigned)(millis() - g_proxy_leases[i].taken_ms));
      g_proxy_leases[i].taken_ms = millis();
      return i;
    }
  }
  return -1;
}

void releaseProxyFile(int idx) {
  if (idx < 0 || idx > 1) return;
  g_proxy_leases[idx].held.store(false);
}

// Releases the lease unless it was handed over to the response's disconnect callback.
struct LeaseGuard {
  explicit LeaseGuard(int i) : idx(i) {}
  ~LeaseGuard() {
    if (idx >= 0) releaseProxyFile(idx);
  }
  int handOver() {
    int i = idx;
    idx = -1;
    return i;
  }
  int idx;
};

void runFetchJob(const ProxyJob &job) {
  std::string url = job.kind == ProxyKind::Stops ? septaStopsUrl(job.param) : transit::septaBusSchedulesUrl(job.param);

  int lease = acquireProxyFile();
  if (lease < 0) {
    // Both files are being streamed to other clients. Telling the caller to try again is the only
    // honest answer; the alternative is overwriting a file someone is reading (F11).
    if (auto r = lockRequest(job)) sendError(r.get(), 503, "busy, try again");
    return;
  }
  LeaseGuard guard(lease);
  const char *path = kProxyFiles[lease];

  int status = -1;
  size_t written = 0;
  bool overflowed = false, write_failed = false, open_failed = false;
  uint8_t head[8] = {0};
  // Coalesce the ~1.4 KB TCP-sized chunks into 4 KB LittleFS writes: each write() is a flash
  // program cycle, and per-chunk writes made a 12 KB Stops list take 16 s end to end.
  static uint8_t wbuf[4096];

  for (int attempt = 0; attempt < 4; ++attempt) {
    File out = LittleFS.open(path, "w");
    if (!out) {
      open_failed = true;
      break;
    }
    written = 0;
    overflowed = write_failed = false;
    memset(head, 0, sizeof(head));
    size_t wlen = 0;
    auto flushBuf = [&]() {
      if (wlen == 0) return true;
      bool ok = out.write(wbuf, wlen) == wlen;
      wlen = 0;
      return ok;
    };
    uint32_t t0 = millis();
    status = transit_app::get(
        url.c_str(),
        [&](const uint8_t *data, size_t n) {
          if (written < sizeof(head)) {
            size_t take = std::min(sizeof(head) - written, n);
            memcpy(head + written, data, take);
          }
          if (written + n > kProxyFileCap) {
            overflowed = true;
            return false;
          }
          while (n > 0) {
            size_t take = std::min(sizeof(wbuf) - wlen, n);
            memcpy(wbuf + wlen, data, take);
            wlen += take;
            data += take;
            n -= take;
            written += take;
            if (wlen == sizeof(wbuf) && !flushBuf()) {
              write_failed = true;
              return false;
            }
          }
          return true;
        },
        kFetchTimeoutMs);
    if (!write_failed && !flushBuf()) write_failed = true;
    out.close();
    Serial.printf("[proxy] %s -> HTTP %d, %u bytes in %u ms, heap %u\n", url.c_str(), status, (unsigned)written, (unsigned)(millis() - t0), (unsigned)ESP.getFreeHeap());

    // SEPTA's flaky {"error":...} answers for valid ids (transit_core/NOTES.md): retry a few
    // times before giving up. Anything else is final.
    const bool error_shape = written >= 8 && memcmp(head, "{\"error\"", 8) == 0;
    if (!(error_shape && written < 256)) break;
    vTaskDelay(pdMS_TO_TICKS(400 * (attempt + 1)));
  }

  auto req = lockRequest(job);
  if (!req) return;  // client disconnected while the fetch was in flight

  if (open_failed) {
    sendError(req.get(), 500, "could not open temp file");
    return;
  }
  if (overflowed || write_failed) {
    sendError(req.get(), 502, overflowed ? "upstream response exceeded 64KB" : "temp file write failed");
    return;
  }
  // Judge the body, not the status: SEPTA sometimes labels a valid body HTTP 400/501.
  const bool looks_json = written > 0 && (head[0] == '{' || head[0] == '[') && memcmp(head, "{\"error\"", 8) != 0;
  if (!looks_json) {
    sendError(req.get(), 502, "upstream HTTP " + std::to_string(status));
    return;
  }

  // DESIGN.md SS7: "Return SEPTA's JSON as-is" - streamed from the temp file in small chunks. The
  // lease now belongs to the response: registered BEFORE send() so a response that completes
  // inside send() (a body small enough for one _send) still finds the callback in place.
  int held = guard.handOver();
  req->onDisconnect([held]() { releaseProxyFile(held); });
  req->send(LittleFS, path, "application/json");
}

// DESIGN.md SS9.3: streams the relevant monthly CSV files through StatsAggregator, never loading
// one into RAM (sd_logger::streamLogLines reads fixed-size chunks internally).
void runStatsJob(const ProxyJob &job) {
  transit::Epoch now = (transit::Epoch)time(nullptr);
  transit::Epoch window_end = now;
  transit::Epoch window_start = now - (transit::Epoch)job.days * 86400;

  // Heap-allocated, not a local: StatsAggregator's own size assertion (test_stats) puts it just
  // under 8KB, too large to risk on this task's stack alongside everything else in this call
  // chain (see kWorkerStackBytes's comment) - same reasoning as net_poller.cpp's
  // computeStopSummary().
  std::unique_ptr<transit_stats::StatsAggregator> agg(new (std::nothrow) transit_stats::StatsAggregator(job.param, window_start, window_end));
  if (!agg) {
    if (auto r = lockRequest(job)) r->send(503, "application/json", "{\"error\":\"out of memory, try again\"}");
    return;
  }
  for (const std::string &month : transit_stats::monthsInWindow(window_start, window_end)) {
    std::string filename = month + ".csv";
    streamLogLines(filename, [&](const char *line, size_t len) {
      agg->feedLine(line, len);
      return true;
    });
  }

  auto req = lockRequest(job);
  if (!req) return;  // client disconnected while we were reading the SD card

  JsonDocument doc;
  agg->toJson(doc);
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

// Same streaming pass for GET /api/stats/overview: every stop key and Indego station at once
// (fixed-capacity OverviewAggregator, ~2 KB, heap-allocated for the same stack reason).
void runOverviewJob(const ProxyJob &job) {
  transit::Epoch now = (transit::Epoch)time(nullptr);
  transit::Epoch window_end = now;
  transit::Epoch window_start = now - (transit::Epoch)job.days * 86400;

  // F28 / DESIGN.md SS9.3: the aggregator has 8 stop slots and 3 bike slots, and a 30-day log can
  // easily hold more distinct keys than that because the user is allowed to change stops. Under
  // the old first-seen-wins rule those slots went to the stops they USED to watch and the ones on
  // the screen right now were silently dropped - exactly backwards for a page whose job is to
  // describe the current configuration. So the CURRENT keys are reserved up front, in config
  // order, and appear even with zero rows ("just added, no data yet" is a real answer).
  Config cfg = getActiveConfig();
  std::vector<std::string> stop_keys;
  stop_keys.reserve(cfg.stops.size());
  for (const transit::StopConfig &s : cfg.stops) stop_keys.push_back(s.key);
  std::vector<std::string> bike_keys;
  bike_keys.reserve(cfg.bike.stations.size());
  for (const BikeStation &b : cfg.bike.stations) bike_keys.push_back("indego-" + std::to_string(b.id));

  std::unique_ptr<transit_stats::OverviewAggregator> agg(
      new (std::nothrow) transit_stats::OverviewAggregator(window_start, window_end, stop_keys, bike_keys));
  if (!agg) {
    if (auto r = lockRequest(job)) r->send(503, "application/json", "{\"error\":\"out of memory, try again\"}");
    return;
  }
  for (const std::string &month : transit_stats::monthsInWindow(window_start, window_end)) {
    std::string filename = month + ".csv";
    streamLogLines(filename, [&](const char *line, size_t len) {
      agg->feedLine(line, len);
      return true;
    });
  }
  auto req = lockRequest(job);
  if (!req) return;
  JsonDocument doc;
  agg->toJson(doc);
  String body;
  serializeJson(doc, body);
  req->send(200, "application/json", body);
}

void runJob(const ProxyJob &job) {
  if (!requestStillAlive(job)) return;  // client vanished before we even started
  // Same guard as net_poller.cpp's pollOnce(): a bad_alloc here used to be a reboot (exceptions
  // are enabled in this SDK and nothing caught them). The browser gets a 503 and retries.
  try {
    if (job.kind == ProxyKind::Stats) {
      runStatsJob(job);
    } else if (job.kind == ProxyKind::Overview) {
      runOverviewJob(job);
    } else {
      runFetchJob(job);
    }
  } catch (const std::bad_alloc &) {
    Serial.printf("[proxy] out of memory running a queued job (free %u)\n", (unsigned)ESP.getFreeHeap());
    if (auto r = lockRequest(job)) r->send(503, "application/json", "{\"error\":\"out of memory, try again\"}");
  }
}


void enqueue(AsyncWebServerRequest *request, ProxyKind kind, const std::string &param, int days = kDefaultStatsDays) {
  if (g_queue == nullptr || uxQueueSpacesAvailable(g_queue) == 0) {
    sendError(request, 503, "proxy worker busy, try again");
    return;
  }
  // ESPAsyncWebServer replies 501 "Handler did not handle the request" unless a handler that
  // defers its response pauses the request (request continuation).
  auto *job = new (std::nothrow) ProxyJob{kind, param, days, request->pause()};
  if (job == nullptr || xQueueSend(g_queue, &job, 0) != pdTRUE) {
    delete job;
    if (auto r = request->pause().lock()) r->send(503, "application/json", "{\"error\":\"proxy worker busy, try again\"}");
  }
}

}  // namespace

void startProxyWorker() {
  // No task of its own any more: the net_poller task drains the queue between polls
  // (runQueuedProxyJob), which saved a 10 KB stack on a board that had ~55 KB of heap left.
  if (g_queue == nullptr) {
    g_queue = xQueueCreate(kQueueLen, sizeof(ProxyJob *));
  }
}

bool runQueuedProxyJob() {
  if (g_queue == nullptr) return false;
  ProxyJob *job = nullptr;
  if (xQueueReceive(g_queue, &job, 0) != pdTRUE) return false;
  std::unique_ptr<ProxyJob> owner(job);
  runJob(*owner);
  return true;
}

void queueStopsProxy(AsyncWebServerRequest *request, const std::string &route) {
  enqueue(request, ProxyKind::Stops, route);
}

void queueScheduleProxy(AsyncWebServerRequest *request, const std::string &stop_id) {
  enqueue(request, ProxyKind::Schedule, stop_id);
}

void queueOverviewRequest(AsyncWebServerRequest *request, int days) {
  if (days < 1) days = kDefaultStatsDays;
  if (days > kMaxStatsDays) days = kMaxStatsDays;
  enqueue(request, ProxyKind::Overview, std::string(), days);
}

void queueStatsRequest(AsyncWebServerRequest *request, const std::string &stop_key, int days) {
  if (days < 1) days = 1;
  if (days > kMaxStatsDays) days = kMaxStatsDays;
  enqueue(request, ProxyKind::Stats, stop_key, days);
}

}  // namespace transit_app
