#include "net_poller.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ctime>

#include "http_fetch.h"
#include "status_led.h"

namespace transit_app {

namespace {

// DESIGN.md SS1/SS4.3: TransitView per configured route; this skeleton
// hardcodes route 17 (the owner's stops) since transit_core - which will
// own reading the configured route/stop list - isn't wired in yet.
constexpr char kTransitViewUrl[] = "https://www3.septa.org/api/TransitView/index.php?route=17";
constexpr uint32_t kFetchTimeoutMs = 15000;
constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 1;

SemaphoreHandle_t g_mutex = nullptr;
transit::Snapshot g_snapshot;
PollStatus g_status;
uint32_t g_poll_seconds = 30;

void pollOnce() {
  size_t bytes = 0;
  int status = get(
    kTransitViewUrl,
    [&bytes](const uint8_t *, size_t n) {
      bytes += n;
      return true;
    },
    kFetchTimeoutMs
  );

  bool ok = (status == 200);
  log_i("net_poller: GET %s -> status=%d bytes=%u free_heap=%u", kTransitViewUrl, status, (unsigned)bytes, (unsigned)ESP.getFreeHeap());

  if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    g_status.has_polled = true;
    g_status.ok = ok;
    g_status.last_http_status = status;
    g_status.last_bytes = bytes;
    g_status.last_poll_epoch = (uint32_t)time(nullptr);
    g_status.last_error = ok ? "" : ("HTTP status " + std::to_string(status));

    g_snapshot.generated = (transit::Epoch)g_status.last_poll_epoch;
    g_snapshot.last_poll_ok = ok;
    g_snapshot.last_error = g_status.last_error;
    // TODO(transit_core): decode `bytes`-worth of TransitView JSON above
    // (DESIGN.md SS4.3 fields: trip, VehicleID, late, destination,
    // Direction, next_stop_sequence, estimated_seat_availability,
    // timestamp) together with the GTFS-RT TripUpdates feed (SS4.2) and
    // the configured stop list (config_store::getActiveConfig().stops),
    // via transit_core::merge(), and assign the result to
    // g_snapshot.stops / g_snapshot.alerts here instead of leaving them
    // empty. Note `get()`'s onData callback above currently only counts
    // bytes - a real parser needs to consume them (ideally streaming, per
    // DESIGN.md SS5's memory rules) instead of just tallying a length.
    xSemaphoreGive(g_mutex);
  }

  if (ok) {
    flashPollOk();
    setStatusLed(LedState::Off);
  } else {
    setStatusLed(LedState::Error);
  }
}

void pollerTask(void * /*arg*/) {
  for (;;) {
    pollOnce();
    vTaskDelay(pdMS_TO_TICKS((uint32_t)g_poll_seconds * 1000UL));
  }
}

}  // namespace

void startNetPoller(uint32_t poll_seconds) {
  g_poll_seconds = poll_seconds > 0 ? poll_seconds : 30;
  if (g_mutex == nullptr) {
    g_mutex = xSemaphoreCreateMutex();
  }
  xTaskCreatePinnedToCore(pollerTask, "net_poller", kTaskStackBytes, nullptr, kTaskPriority, nullptr, 0 /* core 0, DESIGN.md SS5 */);
}

transit::Snapshot getSnapshot() {
  transit::Snapshot copy;
  if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    copy = g_snapshot;
    xSemaphoreGive(g_mutex);
  }
  return copy;
}

PollStatus getPollStatus() {
  PollStatus copy;
  if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    copy = g_status;
    xSemaphoreGive(g_mutex);
  }
  return copy;
}

}  // namespace transit_app
