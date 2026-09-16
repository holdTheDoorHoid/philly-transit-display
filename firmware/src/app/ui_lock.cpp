#include "ui_lock.h"

namespace transit_app {

namespace {

// Written once, from the display task itself, before the poller exists; read from every task
// thereafter. A single aligned pointer, so no lock - and the only value it can ever hold is the
// handle of the task that wrote it.
TaskHandle_t g_display_task = nullptr;

// Incremented only on the display task (takeShared() counts a miss only when onDisplayTask()), read
// by the web task for GET /api/debug/ui. One aligned 32-bit word with a single writer: a torn read
// is not possible on this chip and a race would cost one count of a diagnostic, so no lock here
// either - taking one to record that we could not take one would be its own joke.
volatile uint32_t g_ui_lock_misses = 0;

}  // namespace

void noteDisplayTask() {
  g_display_task = xTaskGetCurrentTaskHandle();
}

bool onDisplayTask() {
  return g_display_task != nullptr && xTaskGetCurrentTaskHandle() == g_display_task;
}

bool takeShared(SemaphoreHandle_t m, uint32_t normal_ms) {
  if (m == nullptr) return false;
  if (!onDisplayTask()) {
    return xSemaphoreTake(m, pdMS_TO_TICKS(normal_ms)) == pdTRUE;
  }
  // Zero, not pdMS_TO_TICKS(0): the point is the literal zero-wait branch in xQueueSemaphoreTake(),
  // which returns without ever blocking and therefore without ever donating priority - the one path
  // on which vTaskPriorityDisinheritAfterTimeout() cannot be reached (ui_lock.h).
  if (xSemaphoreTake(m, 0) == pdTRUE) return true;
  g_ui_lock_misses++;
  return false;
}

uint32_t uiLockMisses() {
  return g_ui_lock_misses;
}

}  // namespace transit_app
