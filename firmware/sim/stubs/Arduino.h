// Host stand-in for Arduino-ESP32's <Arduino.h>, for the screenshot simulator (firmware/sim).
// Only what the LVGL screens in src/app/ui/ actually touch: millis()/delay() for the hold-to-reset
// gesture, ESP.restart()/getFreeHeap() for the device page, and the log_* macros. The simulator's
// -Isim/stubs comes first on the include path, so this shadows the real header on the host build
// and nothing else; the ESP32 environments never see this directory.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace sim {
extern uint32_t millis_ms;    // advanced by the simulator's fake clock
extern uint32_t free_heap;    // what ESP.getFreeHeap() reports
extern uint32_t uptime_s;     // what the device page's uptime line reports (millis()/1000 is too short in a sim run)
}  // namespace sim

inline uint32_t millis() { return sim::millis_ms; }
inline void delay(uint32_t ms) { sim::millis_ms += ms; }

struct SimEsp {
  void restart() { std::printf("[sim] ESP.restart() called (ignored)\n"); }
  uint32_t getFreeHeap() { return sim::free_heap; }
};
extern SimEsp ESP;

#define log_e(fmt, ...) std::printf("[E] " fmt "\n", ##__VA_ARGS__)
#define log_w(fmt, ...) std::printf("[W] " fmt "\n", ##__VA_ARGS__)
#define log_i(fmt, ...) std::printf("[I] " fmt "\n", ##__VA_ARGS__)
#define log_d(fmt, ...) ((void)0)
