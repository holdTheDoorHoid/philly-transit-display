#include "heap_reserve.h"

#include <esp_heap_caps.h>

#include <atomic>

namespace transit_app {

namespace {
// Atomic, because the two sides run on different tasks: an error reply frees it from whichever
// task is answering (async_tcp, or the poller for a queued job), and the poller re-arms it. The
// exchange below is what makes "only one of us frees it" true without a lock - and a lock is not
// available here anyway, since one of the callers is a catch handler on the path where the heap
// has already run out.
std::atomic<void *> g_reserve{nullptr};
std::atomic<uint32_t> g_dropped{0};
// Written once in setup(), read by the web task thereafter.
int g_bt_rc = -1;
int32_t g_bt_gain = 0;
}  // namespace

bool heapReserveHeld() { return g_reserve.load() != nullptr; }

bool armHeapReserve() {
  if (g_reserve.load() != nullptr) return true;
  // MALLOC_CAP_8BIT deliberately: the reserve has to be the kind of memory a String and an
  // AsyncBasicResponse can actually come out of, which is the whole distinction DESIGN.md SS2.1
  // draws between ESP.getFreeHeap() and free8.
  void *p = heap_caps_malloc(kHeapReserveBytes, MALLOC_CAP_8BIT);
  if (p == nullptr) return false;
  void *expected = nullptr;
  if (!g_reserve.compare_exchange_strong(expected, p)) {
    heap_caps_free(p);  // someone else armed it first; do not hold two
  }
  return true;
}

bool releaseHeapReserve() {
  void *p = g_reserve.exchange(nullptr);
  if (p == nullptr) return false;
  heap_caps_free(p);
  return true;
}

bool rearmHeapReserveIfSafe() {
  if (!shouldRearmReserve(heapReserveHeld(), heap_caps_get_free_size(MALLOC_CAP_8BIT),
                          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))) {
    return false;
  }
  return armHeapReserve();
}

void noteBluetoothRelease(int rc, int32_t gain_bytes) {
  g_bt_rc = rc;
  g_bt_gain = gain_bytes;
}

int bluetoothReleaseRc() { return g_bt_rc; }

int32_t bluetoothReleaseGainBytes() { return g_bt_gain; }

uint32_t oomRepliesDropped() { return g_dropped.load(); }

void noteOomReplyDropped() { g_dropped.fetch_add(1); }

}  // namespace transit_app
