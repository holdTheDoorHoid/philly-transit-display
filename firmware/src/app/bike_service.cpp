#include "bike_service.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <ctime>

namespace transit_app {

namespace {

constexpr uint32_t kRefreshMs = 5 * 60 * 1000;

SemaphoreHandle_t g_mutex = nullptr;
BikeView g_view;
uint32_t g_fetched_ms = 0;
volatile bool g_invalidate = true;
std::vector<int> g_fetched_ids;  // station ids of the last fetch: a config change mid-poll must not be missed

SemaphoreHandle_t mutex() {
  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
  return g_mutex;
}

}  // namespace

void invalidateBikes() {
  g_invalidate = true;
}

void refreshBikes(const Config &cfg, const transit::HttpGet &http) {
  if (!cfg.bike.enabled || cfg.bike.stations.empty()) {
    if (xSemaphoreTake(mutex(), pdMS_TO_TICKS(500)) == pdTRUE) {
      g_view = BikeView{};
      xSemaphoreGive(mutex());
    }
    return;
  }
  std::vector<int> ids;
  for (const BikeStation &b : cfg.bike.stations) ids.push_back(b.id);
  bool due = g_invalidate || g_fetched_ms == 0 || ids != g_fetched_ids || (millis() - g_fetched_ms) >= kRefreshMs;
  if (!due) return;
  g_invalidate = false;
  g_fetched_ids = ids;
  indego::StatusStream stream;
  stream.setStationFilter(ids);
  int status = http(indego::statusUrl(), [&](const uint8_t *d, size_t n) { return stream.push(d, n); });
  stream.finish();

  BikeView next;
  next.enabled = true;
  for (const BikeStation &b : cfg.bike.stations) {
    indego::Station st;
    st.id = b.id;
    st.name = b.name;
    for (const indego::Station &found : stream.stations()) {
      if (found.id == b.id) {
        st = found;
        if (st.name.empty()) st.name = b.name;
        break;
      }
    }
    next.stations.push_back(st);
  }
  bool any = !stream.stations().empty();
  Serial.printf("[bike] HTTP %d, %u features scanned, %u of %u stations found\n", status, (unsigned)stream.featuresSeen(),
                (unsigned)stream.stations().size(), (unsigned)ids.size());
  g_fetched_ms = millis();  // even on failure: the feed is 400 KB, don't retry every 30 s
  if (xSemaphoreTake(mutex(), pdMS_TO_TICKS(500)) == pdTRUE) {
    if (any) {
      next.fetched_epoch = (uint32_t)time(nullptr);
      g_view = std::move(next);
    } else if (!g_view.enabled) {
      g_view = std::move(next);  // first fetch failed: show the names with unknown counts
    }
    xSemaphoreGive(mutex());
  }
}

BikeView getBikes() {
  BikeView v;
  if (xSemaphoreTake(mutex(), pdMS_TO_TICKS(500)) == pdTRUE) {
    v = g_view;
    xSemaphoreGive(mutex());
  }
  return v;
}

}  // namespace transit_app
