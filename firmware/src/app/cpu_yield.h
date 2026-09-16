// Letting IDLE0 run from a CPU-bound loop on the poller task (DESIGN.md SS12.1).
//
// `CONFIG_ESP_TASK_WDT_PANIC=y` with a 5 s timeout, and the task the watchdog checks on core 0 is
// IDLE0. IDLE0 runs at priority 0; the poller task runs at priority 1 on the same core. So any
// stretch of work on the poller that does not BLOCK starves the watchdog's own feeder, and the
// board panics - not because the work was wrong, but because it was uninterrupted.
//
// THE OBVIOUS TWO CALLS DO NOT WORK, and this is the whole reason this header exists rather than a
// `taskYIELD()` sprinkled at the call sites. `taskYIELD()` asks the scheduler to pick the
// highest-priority READY task, which is the poller itself; it returns immediately and IDLE0 never
// runs. `vTaskDelay(0)` is documented as exactly equivalent to `taskYIELD()` and behaves the same.
// Both are no-ops for this problem. Only a delay that actually puts the caller in the Blocked
// state lets a LOWER-priority task run, so the primitive here is `vTaskDelay(1)` - one tick, and
// `CONFIG_FREERTOS_HZ=1000` in this SDK makes that 1 ms. DESIGN.md SS12.1 already records the same
// fact from the other direction: HTTPClient's `Stream::timedRead()` "yields only to
// same-or-higher-priority tasks, so at the poller's priority 1 it does not let IDLE0 run", which is
// what made a single blocked read panic the watchdog.
//
// WHERE IT IS CALLED FROM, and why that is a mechanism rather than a convention. Every CSV scan in
// this firmware - the stats page's per-stop summaries, `GET /api/stats`, `GET /api/stats/overview`
// and the log export - goes through `sd_logger.cpp streamLogLines()`, which owns the loop. Nobody
// writes that loop themselves, so putting the yield inside it covers every caller that exists and
// every caller that will exist. A job author cannot forget it because a job author never sees it.
//
// AND IT IS MEASURED, because a rule with no observable is a rule that comes back. `stretch()`
// keeps the longest gap between two yields seen since boot, `GET /api/debug/ui` reports it as
// `cpu_stretch_ms_max`, and the device suite checks that it stays well under the 5 s watchdog. A
// future loop that does not yield shows up as a number climbing towards 5,000 instead of as a
// panic backtrace on someone's serial console.
#pragma once

#include <stdint.h>

namespace transit_app {

// How long a loop may run before it owes IDLE0 a tick. 40 ms is 1/125th of the watchdog's 5 s
// budget, so even a dozen consecutive slices that each overshoot are nowhere near it, and the cost
// is one 1 ms tick per 40 ms of work - about 2.4 %, which is invisible next to the 5-8 s a 30-day
// CSV scan already takes.
constexpr uint32_t kCpuYieldIntervalMs = 40;

// The watchdog's own timeout (`CONFIG_ESP_TASK_WDT_TIMEOUT_S` = 5). Quoted here so the arithmetic
// below can be checked against it rather than against a number someone remembered.
constexpr uint32_t kTaskWdtTimeoutMs = 5000;

// A yield interval is only safe if a loop that honours it cannot get near the watchdog even when a
// single slice runs long. The build breaks rather than someone's wall if that stops being true.
static_assert(kCpuYieldIntervalMs * 8 < kTaskWdtTimeoutMs,
              "the yield interval must leave the task watchdog an order of magnitude of headroom");

// Pure, and the only arithmetic worth testing: is a yield due? Written as an unsigned subtraction
// so it stays correct across the 49-day `millis()` wrap, the same way poller_liveness.h's ages do.
constexpr bool cpuYieldDue(uint32_t last_yield_ms, uint32_t now_ms,
                           uint32_t interval_ms = kCpuYieldIntervalMs) {
  return (uint32_t)(now_ms - last_yield_ms) >= interval_ms;
}

#ifndef NATIVE_TEST

// The high-water gap between two yields, in milliseconds, since boot. Compared against
// kTaskWdtTimeoutMs by whoever reads it; see the header comment.
uint32_t cpuStretchMsMax();

// Drop one of these on the stack in front of a long loop and call tick() each time round. It is
// deliberately not a scope guard that yields on destruction: the point is to yield DURING the work,
// not after it.
class CpuYielder {
 public:
  CpuYielder();
  // Yields (blocking for one tick) if kCpuYieldIntervalMs has passed since the last one. Cheap
  // enough to call per CSV line: one millis() read and a comparison on the common path.
  void tick();

 private:
  uint32_t last_yield_ms_;
};

#endif  // NATIVE_TEST

}  // namespace transit_app
