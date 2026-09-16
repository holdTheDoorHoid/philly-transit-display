#include "cpu_yield.h"

#include <Arduino.h>

namespace transit_app {

namespace {
// Written only from the poller task (every CpuYielder lives on it) and read only from the web task
// answering GET /api/debug/ui, so a 32-bit high-water needs no lock: a torn read is not possible on
// this architecture for an aligned word, and a lost update would cost one sample of a statistic.
volatile uint32_t g_stretch_ms_max = 0;
}  // namespace

uint32_t cpuStretchMsMax() { return g_stretch_ms_max; }

CpuYielder::CpuYielder() : last_yield_ms_(millis()) {}

void CpuYielder::tick() {
  const uint32_t now = millis();
  const uint32_t gap = now - last_yield_ms_;  // unsigned: correct across the millis() wrap
  if (gap > g_stretch_ms_max) g_stretch_ms_max = gap;
  if (!cpuYieldDue(last_yield_ms_, now)) return;
  // vTaskDelay(1), NOT taskYIELD() and NOT vTaskDelay(0) - see cpu_yield.h. One tick is 1 ms at
  // this SDK's CONFIG_FREERTOS_HZ=1000, and it is the blocking that matters, not the duration:
  // while this task is Blocked the scheduler has nothing else ready on core 0 above priority 0, so
  // IDLE0 runs and feeds the task watchdog.
  vTaskDelay(1);
  last_yield_ms_ = millis();
}

}  // namespace transit_app
