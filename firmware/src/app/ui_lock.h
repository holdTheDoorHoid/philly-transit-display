// Who may wait on whom, and for how long (DESIGN.md SS5, SS12.1).
//
// The LVGL display task must never block on another task's mutex. That rule was written in SS5 and
// then kept by convention at each call site, which is not a mechanism: a previous pass capped
// getStopSummary() and tryGetPollStatus() at 50 ms and left five other accessors on their original
// 500-1000 ms waits, and one of those - getBikes() from refreshMainScreen() - panicked a device on
// 2026-09-16 in vTaskPriorityDisinheritAfterTimeout. This header is that mechanism: every shared
// accessor takes its lock through takeShared(), which decides the wait from WHO IS ASKING rather
// than from what the call site remembered to pass.
//
// WHY THE DISPLAY TASK'S BUDGET IS ZERO AND NOT "SHORT".
//
// The assert the device hit is reachable on exactly one path. xQueueSemaphoreTake() calls
// vTaskPriorityDisinheritAfterTimeout() only when a take has (a) actually blocked, (b) TIMED OUT,
// and (c) donated priority on the way in. A shorter timeout does not make that rarer - it makes it
// commoner, because a blocked take that would have succeeded at 400 ms now times out at 50. So
// "cap it at 50 ms" reduces how long the screen freezes without making the panic unreachable; it
// narrows the window instead of closing it.
//
// A zero wait closes it. With xTicksToWait == 0 the take returns immediately if the mutex is
// unavailable: it never enters the blocked state, so it never donates priority, and FreeRTOS says
// so itself on that branch - `configASSERT( xInheritanceOccurred == pdFALSE )` - before returning.
// vTaskPriorityDisinheritAfterTimeout() is not merely unlikely from a zero-wait take, it is
// unreachable. The same fact removes the other half of the problem for free: a task that never
// blocks on a lock can never be priority-inverted behind whoever holds it, so the 1 Hz redraw can
// no longer be held up by a poller mid-parse or a web handler mid-serialise.
//
// The price is misses, and the price is paid by LastGood below: on a miss the accessor hands the
// display task the value it last read, so a miss costs one frame of staleness on one field - the
// clock and the arrivals redraw normally around it - and never a blank panel. Three things keep
// misses rare rather than routine: the writers now prepare their new value outside the lock and
// swap it in (net_poller.cpp publishSnapshot(), weather_service.cpp, bike_service.cpp), the
// Snapshot is published as a shared_ptr so a reader's work under the lock is one refcount bump
// instead of a whole-Snapshot copy, and nothing that can allocate is left under any of these
// mutexes on the display path. uiLockMisses() counts what is left and GET /api/debug/ui reports
// it, so "rare" is an observation and not a hope.
//
// Every other task keeps its old, blocking wait. The web task SHOULD wait for a consistent answer:
// it is answering one request, a few milliseconds do not matter to it, and a 503 or a stale field
// would be a worse answer than a short pause. This asymmetry is the whole point of deciding the
// wait from the caller.
#pragma once
#include <cstdint>

namespace transit_app {

// The display task's budget for waiting on a mutex another task can hold. Zero, for the reason
// above; kept as a named constant because DESIGN.md SS5 cites it and because a bare 0 at a call
// site reads like an oversight.
constexpr uint32_t kUiLockWaitMs = 0;

// The wait a caller is allowed. Pure, so `pio test -e native` can cover it: the policy is the part
// worth pinning down, and it is a function of two numbers.
constexpr uint32_t lockWaitMsFor(bool on_display_task, uint32_t normal_ms) {
  return on_display_task ? kUiLockWaitMs : normal_ms;
}

// The last value the display task successfully read, and how many reads in a row have missed since
// then. One of these lives inside each accessor, is touched ONLY when onDisplayTask() is true, and
// therefore needs no lock of its own - it is per-task state that happens to be spelled `static`.
//
// Deliberately not a "cache": it is never consulted by any other task, never shared, and never
// used to skip a read that could have succeeded. It exists for exactly one case - the read was
// refused - and its whole contract is that the caller gets last frame's answer instead of a
// default-constructed one. `misses` is what says how often that happens; a value that keeps
// climbing is a lock that is genuinely always held, which is a bug worth seeing rather than
// hiding.
template <class T>
class LastGood {
 public:
  // Assign into this under the lock, then call hit(). Two steps rather than one setter because the
  // assignment can throw (T holds vectors and strings) and a half-finished copy must not be
  // recorded as a good one.
  T &slot() { return value_; }
  void hit() {
    have_ = true;
    misses_ = 0;
  }
  void miss() {
    if (misses_ != UINT32_MAX) ++misses_;
  }
  bool have() const { return have_; }
  uint32_t misses() const { return misses_; }
  const T &value() const { return value_; }

 private:
  T value_{};
  bool have_ = false;
  uint32_t misses_ = 0;
};

}  // namespace transit_app

// Everything above is pure C++ and is what the host tests include. Below is the FreeRTOS half,
// compiled only on the device.
#ifndef NATIVE_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace transit_app {

// Records the calling task as THE display task. main.cpp setup() calls it on loopTask, which is
// also the LVGL loop, immediately before startNetPoller() - i.e. at the moment there first exists
// another task to contend with. Before that call onDisplayTask() is false and every accessor
// blocks normally, which is what boot wants: there is no contention yet and a blank first frame
// would be worse than a wait.
void noteDisplayTask();
bool onDisplayTask();

// Takes `m` under this task's policy: zero wait on the display task, `normal_ms` everywhere else.
// Returns false if it did not get the lock, having already counted the miss when the caller was the
// display task. A false return is NOT an error - it means "use what you had".
bool takeShared(SemaphoreHandle_t m, uint32_t normal_ms);
inline void giveShared(SemaphoreHandle_t m) { xSemaphoreGive(m); }

// Display-task lock misses since boot, across every accessor. GET /api/debug/ui reports it; the
// device suite watches it stay small, which is how "a miss costs one frame" is checked rather than
// asserted.
uint32_t uiLockMisses();

}  // namespace transit_app
#endif  // NATIVE_TEST
