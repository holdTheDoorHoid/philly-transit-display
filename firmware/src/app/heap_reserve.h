// A kilobyte held back so the device can still say "out of memory" when it is out of memory.
//
// THE CRASH THIS EXISTS FOR (coredump read off the diag board's flash, 78 min uptime, 2026-09-17).
// Task `async_tcp`, "abort() was called":
//
//   abort <- std::terminate <- __cxa_throw <- operator new (sz=100)
//     <- AsyncWebServerRequest::beginResponse        WebRequest.cpp:1212
//     <- AsyncWebServerRequest::send(503, "application/json", "{\"error\":\"out of memory, retry\"}")
//     <- the catch handler in transit_app::guarded() src/app/web_server.cpp:1330
//
// Read it from the bottom up: a handler threw std::bad_alloc, guarded() caught it exactly as
// designed, and then the 503 it tried to answer with ALLOCATED - `beginResponse` does
// `new AsyncBasicResponse(...)`, about 100 bytes, plus a String for the content type and one for
// the body. That second allocation failed too, and a throw that escapes a catch handler with
// nothing outside it is std::terminate. DESIGN.md SS12.1 already named this as one of the things
// the emergency exception pool cannot help with - "a catch block that itself allocates with
// nothing left" - and this is that case, in our own code, on the path that runs precisely when the
// heap has troughed (min_free8 since boot on that board: 1,452 B).
//
// TWO LAYERS, because neither is sufficient alone.
//
// 1. THE RESERVE (this file). One 1,024 B block is taken at boot, before Wi-Fi, and simply held.
//    Every reply that exists BECAUSE memory ran out - guarded()'s 503, refuseIfLowHeap()'s 503,
//    the 401/429s, the truncated-document 503s, the Host-check 421 - frees it immediately BEFORE
//    calling request->send(), so the response object has somewhere to come from. It is re-armed
//    later, from a context that is not the handler that just spent it: the poller's idle slice and
//    the top of a poll cycle, and only when the heap is comfortably clear of the gate (below).
//    At rest it is 1 KB that is deliberately unavailable, which is the price.
//
// 2. A NESTED try/catch AROUND THE SEND (oom_reply.h). A throw inside a catch handler IS catchable
//    by a try block nested inside that handler, so the send is wrapped; if it still fails, the
//    connection is closed with no reply and `oom_replies_dropped` counts it on
//    GET /api/debug/ui. The web app's resilientRead() (DESIGN.md SS10.2) already treats a dropped
//    connection as a retryable failure, so a closed socket is a worse answer than a 503 and a far
//    better one than a reboot.
//
// The arm/disarm POLICY is pure and lives here so it can be host-tested
// (`pio test -e native -f test_heap_reserve`), the same shape as poller_liveness.h and
// proxy_queue.h; the block itself is in heap_reserve.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace transit_app {

// Enough for what an error reply costs: AsyncBasicResponse is ~100 B, plus an Arduino String for
// the content type and one for the body (these replies are fixed literals of 30-60 characters),
// plus allocator headers - a few hundred bytes in all. 1 KB is that with room to be wrong.
constexpr size_t kHeapReserveBytes = 1024;

// When it is safe to take the kilobyte back. Deliberately well ABOVE the gates the reserve exists
// to get past (kMinHeavyResponseFree8 12 KB / kMinHeavyResponseBlock 7,924 B in web_server.cpp):
// re-arming must never be the allocation that pushes a device below the floor it just recovered
// over, and there is no hurry - the reserve is only needed on a path that is already failing.
constexpr size_t kHeapReserveRearmFree8 = 20 * 1024;
// 4,340 and not 4,096: largest-block readings land on a 512-byte lattice at offset 500 (DESIGN.md
// SS2.1), so a round 4 KB sits 4 B below a real resting value of 4,084 and 244 B under the next one
// up. 756 + 512k is mid-gap, which is where a threshold belongs.
constexpr size_t kHeapReserveRearmBlock = 4340;

// The whole policy, as arithmetic over three inputs. `held` is whether the block is currently
// held; `free8`/`largest` are MALLOC_CAP_8BIT readings.
constexpr bool shouldRearmReserve(bool held, size_t free8, size_t largest) {
  return !held && free8 >= kHeapReserveRearmFree8 && largest >= kHeapReserveRearmBlock;
}

// Is the block currently held?
bool heapReserveHeld();

// Takes the block. Call once from setup(), before Wi-Fi, out of a heap that is still one run.
// Returns true if the block is held afterwards (including "it already was").
bool armHeapReserve();

// Gives the block back to the allocator. Returns true if this call is what freed it. Call it
// immediately before an error reply's request->send(), and nowhere else.
bool releaseHeapReserve();

// Re-arms if shouldRearmReserve() says so. Returns true if this call took the block. Call only
// from a context that is NOT an OOM reply path - the poller's idle slice and poll-start are the
// two used here.
bool rearmHeapReserveIfSafe();

// ---- The Bluetooth-memory release, as a readable fact rather than a serial line ---------------
//
// main.cpp calls esp_bt_mem_release(ESP_BT_MODE_BTDM) once, first thing in setup(), to hand
// libbt's `_bt_data` (4,464 B of .dram0.data on this image) back to the heap - see the comment
// there and audit_static SS4.1. The call was read out of the disassembly and had never run on this
// hardware, so it records what happened where it can be READ over HTTP: the serial console cannot
// be captured on this bench, because opening the port resets the board.
//
// `rc` is the esp_err_t it returned (0 = ESP_OK), -1 until the call has been made or if the build
// has no Bluetooth controller at all. `gain` is the MALLOC_CAP_8BIT free-heap delta across the
// call, which is the number that says whether the region actually joined the heap.
void noteBluetoothRelease(int rc, int32_t gain_bytes);
int bluetoothReleaseRc();
int32_t bluetoothReleaseGainBytes();

// How many error replies could not be sent at all, since boot, and ended in a closed connection
// instead. Reported as `oom_replies_dropped` by GET /api/debug/ui. It should be zero; a number
// that moves means the heap reached a state where even a fixed-literal 503 would not fit.
uint32_t oomRepliesDropped();
void noteOomReplyDropped();

}  // namespace transit_app
