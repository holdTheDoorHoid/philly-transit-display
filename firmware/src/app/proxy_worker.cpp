#include "proxy_worker.h"

#include <Arduino.h>

#include <new>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
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

// DESIGN.md task brief: "fetch into a bounded buffer (cap 32 KB; Stops is ~12 KB)".
constexpr size_t kBodyCap = 32 * 1024;
constexpr uint32_t kFetchTimeoutMs = 15000;
constexpr UBaseType_t kQueueLen = 4;
// StatsAggregator alone can be just under 8KB (its own size assertion in test_stats), so this
// must comfortably clear that plus the rest of runStatsJob()'s frame (JsonDocument, String,
// lambdas) even with it heap-allocated rather than a local - a smaller stack here silently
// corrupts memory instead of failing loudly, so err generous.
constexpr uint32_t kWorkerStackBytes = 8192;  // TLS handshake needs ~5 KB; JSON docs and the aggregator live on the heap
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
  AsyncWebServerRequest *request;
  std::shared_ptr<std::atomic<bool>> alive;
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
  return job.alive->load() && job.request->client() != nullptr && job.request->client()->connected();
}

void sendError(AsyncWebServerRequest *request, int code, const std::string &message) {
  request->send(code, "application/json", (std::string("{\"error\":\"") + message + "\"}").c_str());
}

void runFetchJob(const ProxyJob &job) {
  std::string url = job.kind == ProxyKind::Stops ? septaStopsUrl(job.param) : transit::septaBusSchedulesUrl(job.param);
  bool tls_verify = getActiveConfig().device.tls_verify;

  std::vector<uint8_t> body;
  bool overflowed = false;
  int status = transit_app::get(
      url.c_str(),
      [&](const uint8_t *data, size_t n) {
        if (body.size() + n > kBodyCap) {
          overflowed = true;
          return false;
        }
        body.insert(body.end(), data, data + n);
        return true;
      },
      kFetchTimeoutMs, tls_verify);

  if (!requestStillAlive(job)) return;  // client disconnected while the fetch was in flight

  if (overflowed) {
    sendError(job.request, 502, "upstream response exceeded 32KB");
    return;
  }
  if (status != 200 || body.empty()) {
    sendError(job.request, 502, "upstream HTTP " + std::to_string(status));
    return;
  }

  // DESIGN.md SS7: "Return SEPTA's JSON as-is" - no re-parsing, just forward the bytes.
  AsyncWebServerResponse *response =
      job.request->beginResponse(200, "application/json", body.data(), body.size());
  job.request->send(response);
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
    if (requestStillAlive(job)) job.request->send(503, "application/json", "{\"error\":\"out of memory, try again\"}");
    return;
  }
  for (const std::string &month : transit_stats::monthsInWindow(window_start, window_end)) {
    std::string filename = month + ".csv";
    streamLogLines(filename, [&](const char *line, size_t len) {
      agg->feedLine(line, len);
      return true;
    });
  }

  if (!requestStillAlive(job)) return;  // client disconnected while we were reading the SD card

  JsonDocument doc;
  agg->toJson(doc);
  String body;
  serializeJson(doc, body);
  job.request->send(200, "application/json", body);
}

void runJob(const ProxyJob &job) {
  if (!requestStillAlive(job)) return;  // client vanished before we even started
  if (job.kind == ProxyKind::Stats) {
    runStatsJob(job);
  } else {
    runFetchJob(job);
  }
}

void workerTask(void * /*arg*/) {
  for (;;) {
    ProxyJob *job = nullptr;
    if (xQueueReceive(g_queue, &job, portMAX_DELAY) == pdTRUE) {
      std::unique_ptr<ProxyJob> owner(job);
      runJob(*owner);
    }
  }
}

void enqueue(AsyncWebServerRequest *request, ProxyKind kind, const std::string &param, int days = kDefaultStatsDays) {
  auto alive = std::make_shared<std::atomic<bool>>(true);
  request->onDisconnect([alive]() { alive->store(false); });

  auto *job = new ProxyJob{kind, param, days, request, alive};
  if (g_queue == nullptr || xQueueSend(g_queue, &job, 0) != pdTRUE) {
    delete job;
    sendError(request, 503, "proxy worker busy, try again");
  }
}

}  // namespace

void startProxyWorker() {
  if (g_queue == nullptr) {
    g_queue = xQueueCreate(kQueueLen, sizeof(ProxyJob *));
  }
  static bool started = false;
  if (started) return;  // main.cpp starts it early (before Wi-Fi) so the stack is one contiguous block
  started = true;
  xTaskCreate(workerTask, "proxy_worker", kWorkerStackBytes, nullptr, 1, nullptr);
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
