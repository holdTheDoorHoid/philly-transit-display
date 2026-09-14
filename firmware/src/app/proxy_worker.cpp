#include "proxy_worker.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <vector>

#include "config_store.h"
#include "http_fetch.h"
#include "transit_core/septa_source.h"

namespace transit_app {

namespace {

// DESIGN.md task brief: "fetch into a bounded buffer (cap 32 KB; Stops is ~12 KB)".
constexpr size_t kBodyCap = 32 * 1024;
constexpr uint32_t kFetchTimeoutMs = 15000;
constexpr UBaseType_t kQueueLen = 4;
constexpr uint32_t kWorkerStackBytes = 8192;

enum class ProxyKind { Stops, Schedule };

// Shared between the web-server task (which creates this on a queued request) and this file's
// worker task (which reads it after a possibly-long network fetch): a request whose client has
// gone away must never be touched again (its AsyncWebServerRequest may already be freed), so
// onDisconnect() flips this instead of deleting anything out from under the worker.
struct ProxyJob {
  ProxyKind kind;
  std::string param;  // route (Stops) or stop_id (Schedule)
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

void runJob(const ProxyJob &job) {
  if (!requestStillAlive(job)) return;  // client vanished before we even started

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

void workerTask(void * /*arg*/) {
  for (;;) {
    ProxyJob *job = nullptr;
    if (xQueueReceive(g_queue, &job, portMAX_DELAY) == pdTRUE) {
      std::unique_ptr<ProxyJob> owner(job);
      runJob(*owner);
    }
  }
}

void enqueue(AsyncWebServerRequest *request, ProxyKind kind, const std::string &param) {
  auto alive = std::make_shared<std::atomic<bool>>(true);
  request->onDisconnect([alive]() { alive->store(false); });

  auto *job = new ProxyJob{kind, param, request, alive};
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
  xTaskCreate(workerTask, "proxy_worker", kWorkerStackBytes, nullptr, 1, nullptr);
}

void queueStopsProxy(AsyncWebServerRequest *request, const std::string &route) {
  enqueue(request, ProxyKind::Stops, route);
}

void queueScheduleProxy(AsyncWebServerRequest *request, const std::string &stop_id) {
  enqueue(request, ProxyKind::Schedule, stop_id);
}

}  // namespace transit_app
