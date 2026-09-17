#include "heap_trace.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace transit_app {

const char *const kHeapTraceStages[kHeapTraceStageCount] = {
    "poll-start",   "pre-transit",  "rt-stream",    "tv-route",     "sched-stop",  "merge",
    "oom-transit",  "post-transit", "pre-alerts",   "post-alerts",  "pre-weather", "post-weather",
    "pre-bikes",    "post-bikes",   "pre-liveness", "post-liveness", "post-tracker", "post-sd",
    "cycle-end",    "oom-cycle",    "oom-bikes",    "oom-proxy",    "oom-status",
};
static_assert(sizeof(kHeapTraceStages) / sizeof(kHeapTraceStages[0]) == kHeapTraceStageCount,
              "kHeapTraceStages must carry one name per HeapTraceStage id");

namespace {

// .bss, not the heap - the whole point (heap_trace.h). 64 * 12 = 768 B.
HeapTraceEntry g_ring[kHeapTraceCap];
uint32_t g_seq = 0;    // total marks since boot; g_ring[k % cap] holds sequence number k
uint16_t g_cycle = 0;  // pollOnce() cycles since boot

// The marks come from the poller task, the AsyncTCP task (the proxy catch) and the display task
// (the bike/status catches), and the readout comes from the AsyncTCP task, so the 12-byte store and
// the index bump need to be atomic with respect to each other. A portMUX spinlock and not a mutex:
// nothing inside the critical section can block, and a mutex here could be taken by the very task
// whose allocation failure we are recording.
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

void heapTraceMark(uint8_t stage) {
  // Sampled OUTSIDE the critical section on purpose: heap_caps_* take the heap's own lock, and
  // taking a blocking lock inside a portMUX section is exactly the thing that is not allowed. The
  // two numbers are therefore read a few microseconds before they are filed, which is far finer
  // than anything this measurement is trying to resolve.
  const uint32_t free8 = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const uint32_t largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  portENTER_CRITICAL(&g_lock);
  HeapTraceEntry &e = g_ring[g_seq % kHeapTraceCap];
  e.cycle = g_cycle;
  e.stage = stage;
  e.flags = 0;
  e.free8 = free8;
  e.largest = largest;
  g_seq++;
  portEXIT_CRITICAL(&g_lock);
}

void heapTraceBeginCycle() {
  portENTER_CRITICAL(&g_lock);
  g_cycle++;
  portEXIT_CRITICAL(&g_lock);
  heapTraceMark(kStagePollStart);
}

uint32_t heapTraceSeq() {
  portENTER_CRITICAL(&g_lock);
  const uint32_t s = g_seq;
  portEXIT_CRITICAL(&g_lock);
  return s;
}

size_t heapTraceRead(uint32_t since, HeapTraceEntry *out, size_t max, uint32_t *out_first_seq) {
  if (out == nullptr || max == 0) {
    if (out_first_seq != nullptr) *out_first_seq = 0;
    return 0;
  }
  portENTER_CRITICAL(&g_lock);
  const uint32_t seq = g_seq;
  // The oldest sequence number the ring still holds.
  const uint32_t oldest = seq > (uint32_t)kHeapTraceCap ? seq - (uint32_t)kHeapTraceCap : 0;
  uint32_t first = since > oldest ? since : oldest;
  // A `since` at or past the newest mark asks for nothing, and must not be allowed to underflow the
  // subtraction below into "give me the whole ring".
  if (first > seq) first = seq;
  // Honour `max` by dropping the OLDEST rows, not the newest: on the failure path the newest cycle
  // is the one being diagnosed, and a reader that wants the rest asks again with a smaller `since`.
  if (seq - first > (uint32_t)max) first = seq - (uint32_t)max;
  size_t n = 0;
  for (uint32_t k = first; k < seq && n < max; ++k, ++n) {
    out[n] = g_ring[k % kHeapTraceCap];
  }
  portEXIT_CRITICAL(&g_lock);
  if (out_first_seq != nullptr) *out_first_seq = first;
  return n;
}

}  // namespace transit_app
