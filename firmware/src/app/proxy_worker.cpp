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

enum class ProxyKind { Stops, Schedule, Stats };

// Shared between the web-server task (which creates this on a queued request) and this file's
// worker task (which reads it after a possibly-long network fetch or SD scan): a request whose
// client has gone away must never be touched again (its AsyncWebServerRequest may already be
// freed), so onDisconnect() flips this instead of deleting anything out from under the worker.
struct ProxyJob {
  ProxyKind kind;
  std::string param;  // route (Stops), stop_id (Schedule), or stop key (Stats)
  int days = kDefaultStatsDays;  // Stats only
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
uint8_t g_proxy_file_idx = 0;

void runFetchJob(const ProxyJob &job) {
  std::string url = job.kind == ProxyKind::Stops ? septaStopsUrl(job.param) : transit::septaBusSchedulesUrl(job.param);
  bool tls_verify = getActiveConfig().device.tls_verify;

  const char *path = kProxyFiles[g_proxy_file_idx];
  g_proxy_file_idx ^= 1;  // alternate so a response still being streamed is not overwritten
  File out = LittleFS.open(path, "w");
  if (!out) {
    if (auto r = lockRequest(job)) sendError(r.get(), 500, "could not open temp file");
    return;
  }

  size_t written = 0;
  bool overflowed = false, write_failed = false;
  uint8_t head[8] = {0};
  // Coalesce the ~1.4 KB TCP-sized chunks into 4 KB LittleFS writes: each write() is a flash
  // program cycle, and per-chunk writes made a 12 KB Stops list take 16 s end to end.
  static uint8_t wbuf[4096];
  size_t wlen = 0;
  auto flushBuf = [&]() {
    if (wlen == 0) return true;
    bool ok = out.write(wbuf, wlen) == wlen;
    wlen = 0;
    return ok;
  };
  uint32_t t0 = millis();
  int status = transit_app::get(
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
          wlen += take; data += take; n -= take; written += take;
          if (wlen == sizeof(wbuf) && !flushBuf()) {
            write_failed = true;
            return false;
          }
        }
        return true;
      },
      kFetchTimeoutMs, tls_verify);
  if (!write_failed && !flushBuf()) write_failed = true;
  out.close();
  Serial.printf("[proxy] %s -> HTTP %d, %u bytes in %u ms, heap %u\n", url.c_str(), status, (unsigned)written, (unsigned)(millis() - t0), (unsigned)ESP.getFreeHeap());

  auto req = lockRequest(job);
  if (!req) return;  // client disconnected while the fetch was in flight

  if (overflowed || write_failed) {
    sendError(req.get(), 502, overflowed ? "upstream response exceeded 64KB" : "temp file write failed");
    return;
  }
  // SEPTA intermittently answers 400/501 with a perfectly valid body (transit_core/NOTES.md), so
  // judge the body: forward anything that looks like JSON and is not its {"error": ...} shape.
  const bool looks_json = written > 0 && (head[0] == '{' || head[0] == '[') && memcmp(head, "{\"error\"", 8) != 0;
  if (!looks_json) {
    sendError(req.get(), 502, "upstream HTTP " + std::to_string(status));
    return;
  }

  // DESIGN.md SS7: "Return SEPTA's JSON as-is" - streamed from the temp file in small chunks.
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

void runJob(const ProxyJob &job) {
  if (!requestStillAlive(job)) return;  // client vanished before we even started
  if (job.kind == ProxyKind::Stats) {
    runStatsJob(job);
  } else {
    runFetchJob(job);
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

void queueStatsRequest(AsyncWebServerRequest *request, const std::string &stop_key, int days) {
  if (days < 1) days = 1;
  if (days > kMaxStatsDays) days = kMaxStatsDays;
  enqueue(request, ProxyKind::Stats, stop_key, days);
}

}  // namespace transit_app
