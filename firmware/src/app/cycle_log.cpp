#include "cycle_log.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace transit_app {

namespace {

// .bss, not the heap - the whole point (cycle_log.h). 240 * 16 = 3,840 B.
CycleLogEntry g_ring[kCycleLogCap];
uint32_t g_seq = 0;  // rows filed since boot; g_ring[k % cap] holds sequence number k

// The row being accumulated. `open` is false before the first cycle and again immediately after a
// row is filed, so a stray cycleLogFlag() from a task that is late cannot write into a row that
// has already gone out.
CycleLogEntry g_open;
bool g_open_valid = false;
uint32_t g_open_min_free8 = 0xFFFFFFFFu;

// Same reasoning as heap_trace.cpp's lock: the writers are the poller task, the AsyncTCP task (the
// accept path and the proxy catch) and the display task, the reader is the AsyncTCP task, and
// nothing inside the critical section can block. A mutex here could be taken by the very task
// whose allocation failure is being recorded.
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

// Files g_open into the ring. Caller holds g_lock.
void fileOpenLocked() {
  if (!g_open_valid) return;
  uint32_t scaled = g_open_min_free8 == 0xFFFFFFFFu ? g_open.free8 : g_open_min_free8;
  scaled /= kMinFreeScale;
  g_open.min_free8_64 = scaled > 0xFFFFu ? 0xFFFFu : (uint16_t)scaled;
  g_ring[g_seq % kCycleLogCap] = g_open;
  g_seq++;
  g_open_valid = false;
}

}  // namespace

void cycleLogBegin(uint32_t free8, uint32_t largest) {
  portENTER_CRITICAL(&g_lock);
  // A cycle that ended without cycleLogEnd() - there is no such path today, but a future one would
  // otherwise silently lose its row - is filed here rather than overwritten.
  fileOpenLocked();
  g_open.uptime_s = (uint32_t)(millis() / 1000U);
  g_open.free8 = free8;
  g_open.largest = largest;
  g_open.min_free8_64 = 0;
  g_open.flags = 0;
  g_open_min_free8 = free8;
  g_open_valid = true;
  portEXIT_CRITICAL(&g_lock);
}

void cycleLogNoteFree8(uint32_t free8) {
  portENTER_CRITICAL(&g_lock);
  if (g_open_valid && free8 < g_open_min_free8) g_open_min_free8 = free8;
  portEXIT_CRITICAL(&g_lock);
}

void cycleLogFlag(uint16_t flags) {
  portENTER_CRITICAL(&g_lock);
  if (g_open_valid) g_open.flags |= flags;
  portEXIT_CRITICAL(&g_lock);
}

void cycleLogEnd() {
  portENTER_CRITICAL(&g_lock);
  fileOpenLocked();
  portEXIT_CRITICAL(&g_lock);
}

uint32_t cycleLogSeq() {
  portENTER_CRITICAL(&g_lock);
  const uint32_t s = g_seq;
  portEXIT_CRITICAL(&g_lock);
  return s;
}

size_t cycleLogRead(uint32_t since, CycleLogEntry *out, size_t max, uint32_t *out_first_seq) {
  if (out == nullptr || max == 0) {
    if (out_first_seq != nullptr) *out_first_seq = 0;
    return 0;
  }
  portENTER_CRITICAL(&g_lock);
  const uint32_t seq = g_seq;
  const uint32_t oldest = seq > (uint32_t)kCycleLogCap ? seq - (uint32_t)kCycleLogCap : 0;
  uint32_t first = since > oldest ? since : oldest;
  // A `since` at or past the newest row asks for nothing, and must not underflow the subtraction
  // below into "give me the whole ring".
  if (first > seq) first = seq;
  // Honour `max` by dropping the OLDEST rows: a reader catching up asks again with a smaller
  // `since`, and the newest rows are the ones a live investigation wants first.
  if (seq - first > (uint32_t)max) first = seq - (uint32_t)max;
  size_t n = 0;
  for (uint32_t k = first; k < seq && n < max; ++k, ++n) {
    out[n] = g_ring[k % kCycleLogCap];
  }
  portEXIT_CRITICAL(&g_lock);
  if (out_first_seq != nullptr) *out_first_seq = first;
  return n;
}

}  // namespace transit_app
