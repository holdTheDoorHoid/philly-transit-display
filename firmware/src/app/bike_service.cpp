#include "bike_service.h"

#include "cycle_log.h"

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

// 10 minutes, not 5, since 0.3.1 (owner decision, 2026-09-17). Each refresh streams a ~400 KB
// GeoJSON body through this task; on the measured ring the bikes stage is the single most
// expensive thing a cycle does (post-bikes free8 -17.6 KB, largest block down to 6.4 KB, held to
// cycle end). Halving how often it happens halves how often the cycle spends that, and dock counts
// at a station do not move meaningfully in ten minutes. The FIRST fetch of a boot is unaffected -
// g_fetched_ms == 0 below makes it due immediately - and a config change still forces one through
// invalidateBikes(). The hourly `bike` log rows stay inside kBikeSampleMaxAgeS (15 min,
// net_poller.cpp), which a 10-minute cadence clears with 5 minutes to spare.
constexpr uint32_t kRefreshMs = 10 * 60 * 1000;
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
// 6,144 B feature buffer is BORROWED from the poller's shared scratch (refreshBikes below), so the
// object itself is about a hundred bytes: the bytes were already paid for by the transit fetches
// earlier in the same cycle, which are done with them by now. Heap-allocated rather than a
// file-scope object for the same reason the ArrivalTracker is: the ESP32's static .bss budget is
// separate from and much smaller than the heap.
indego::StatusStream *g_stream = nullptr;

}  // namespace

bool preallocateBikeStream(std::vector<uint8_t> *scratch) {
  if (g_stream == nullptr) g_stream = new (std::nothrow) indego::StatusStream();
  // Immediately, not on the first refresh: the constructor reserves its own 6 KB feature buffer,
  // and handing the borrow over here releases that instead of leaving it held through the whole
  // first cycle. setFeatureBuffer(nullptr) is a no-op, so the fallback path is unchanged.
  if (g_stream != nullptr && scratch != nullptr) g_stream->setFeatureBuffer(scratch);
  return g_stream != nullptr;
}

void invalidateBikes() {
  g_invalidate = true;
}

void refreshBikes(const Config &cfg, const transit::HttpGet &http, std::vector<uint8_t> *scratch) {
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
  // Borrow the poller's shared buffer for the one-feature scratch. Order matters: setFeatureBuffer
  // gives back whatever the scanner was holding, and reset() then sizes the borrowed vector.
  stream.setFeatureBuffer(scratch);
  stream.reset();
  stream.setStationFilter(ids);
  cycleLogFlag(kCycleBikes);  // this cycle streamed the ~400 KB Indego feed (cycle_log.h)
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
