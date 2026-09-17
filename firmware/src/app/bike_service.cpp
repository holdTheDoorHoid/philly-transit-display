#include "bike_service.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <ctime>
#include <memory>
#include <new>
#include <utility>

#include "heap_trace.h"
#include "ui_lock.h"

namespace transit_app {

namespace {

constexpr uint32_t kRefreshMs = 5 * 60 * 1000;
// What a NON-display task waits for the view. The display task's wait is zero, decided by
// takeShared() (ui_lock.h): getBikes() at 500 ms, called from refreshMainScreen() on every tick the
// arrivals page is up, is the accessor that panicked the board on 2026-09-16 in
// vTaskPriorityDisinheritAfterTimeout (DESIGN.md SS12.1).
constexpr uint32_t kBikeLockWaitMs = 500;

SemaphoreHandle_t g_mutex = nullptr;
BikeView g_view;
uint32_t g_fetched_ms = 0;
volatile bool g_invalidate = true;
std::vector<int> g_fetched_ids;  // station ids of the last fetch: a config change mid-poll must not be missed

SemaphoreHandle_t mutex() {
  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
  return g_mutex;
}

// The scanner, allocated once before Wi-Fi (preallocateBikeStream) and reset per refresh. Its
// feature buffer is a 6,144 B contiguous block; constructing a StatusStream per refresh asked for
// that block every five minutes, in the middle of a cycle, on a board whose largest free block
// rests at 25-28 KB and decays (DESIGN.md SS5, SS12.1). Heap-allocated rather than a file-scope
// object for the same reason the ArrivalTracker is: the ESP32's static .bss budget is separate
// from and much smaller than the heap.
indego::StatusStream *g_stream = nullptr;

}  // namespace

bool preallocateBikeStream() {
  if (g_stream == nullptr) g_stream = new (std::nothrow) indego::StatusStream();
  return g_stream != nullptr;
}

void invalidateBikes() {
  g_invalidate = true;
}

void refreshBikes(const Config &cfg, const transit::HttpGet &http) {
  if (!cfg.bike.enabled || cfg.bike.stations.empty()) {
    BikeView empty;  // swapped out and destroyed below, with the lock released
    if (xSemaphoreTake(mutex(), pdMS_TO_TICKS(kBikeLockWaitMs)) == pdTRUE) {
      g_view.stations.swap(empty.stations);
      g_view.enabled = false;
      g_view.fetched_epoch = 0;
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
  // The long-lived scanner when there is one - reset(), not constructed, so its 6 KB feature
  // buffer is not asked for again. `own` is the fallback for a board where the pre-allocation
  // failed, and is only ever constructed on that path.
  std::unique_ptr<indego::StatusStream> own;
  if (g_stream == nullptr) own.reset(new (std::nothrow) indego::StatusStream());
  indego::StatusStream *sp = g_stream != nullptr ? g_stream : own.get();
  if (sp == nullptr) {
    Serial.println("[bike] no feed scanner (out of memory); skipping this refresh");
    return;
  }
  indego::StatusStream &stream = *sp;
  stream.reset();
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
  // The 400 KB feed was scanned and `next` was built with NOTHING held - that was already true, and
  // it is the shape the rest of this pass copies. What is under the lock is a swap: even a
  // move-assign would run the outgoing stations' destructors inside the critical section, and the
  // reader that waits on this lock is the one that must never wait (DESIGN.md SS5).
  if (xSemaphoreTake(mutex(), pdMS_TO_TICKS(kBikeLockWaitMs)) == pdTRUE) {
    if (any) {
      next.fetched_epoch = (uint32_t)time(nullptr);
      std::swap(g_view, next);
    } else if (!g_view.enabled) {
      std::swap(g_view, next);  // first fetch failed: show the names with unknown counts
    }
    xSemaphoreGive(mutex());
  }
  // `next` now holds the outgoing view and is destroyed here, outside the lock.
}

BikeView getBikes() {
  static LastGood<BikeView> ui_last;  // display task only (ui_lock.h)
  const bool ui = onDisplayTask();
  BikeView v;
  if (!takeShared(mutex(), kBikeLockWaitMs)) {
    if (ui) {
      ui_last.miss();
      return ui_last.value();  // last frame's counts, not an empty strip that hides the panel
    }
    return v;
  }
  try {
    v = g_view;  // copies a string per station: small, but it allocates, so it can throw
  } catch (const std::bad_alloc &) {
    heapTraceMark(kStageOomBikes);  // diag branch
    giveShared(mutex());
    throw;  // loop()'s guard in main.cpp skips the frame; guarded() answers 503 on the web task
  }
  giveShared(mutex());
  if (ui) {
    ui_last.slot() = v;
    ui_last.hit();
  }
  return v;
}

}  // namespace transit_app
