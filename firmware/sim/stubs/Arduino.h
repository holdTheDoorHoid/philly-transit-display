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
extern uint32_t millis_ms;    // advanced by the simulator's fake clock; starts days in so uptime reads like a device's
extern uint32_t free_heap;    // what ESP.getFreeHeap() reports
}  // namespace sim

inline uint32_t millis() { return sim::millis_ms; }
inline void delay(uint32_t ms) { sim::millis_ms += ms; }

// Enough of HardwareSerial for the screens' own diagnostic lines (the LVGL pool guard in
// main_screen.cpp logs which stop panels it had to leave off). Straight to stdout, so the pool
// sweep shows them inline.
struct SimSerial {
  template <typename... A> void printf(const char *fmt, A... a) { std::printf(fmt, a...); }
  void println(const char *s) { std::printf("%s\n", s); }
  void print(const char *s) { std::printf("%s", s); }
  void flush() { std::fflush(stdout); }
};
extern SimSerial Serial;

struct SimEsp {
  void restart() { std::printf("[sim] ESP.restart() called (ignored)\n"); }
  uint32_t getFreeHeap() { return sim::free_heap; }
};
extern SimEsp ESP;

#define log_e(fmt, ...) std::printf("[E] " fmt "\n", ##__VA_ARGS__)
#define log_w(fmt, ...) std::printf("[W] " fmt "\n", ##__VA_ARGS__)
#define log_i(fmt, ...) std::printf("[I] " fmt "\n", ##__VA_ARGS__)
#define log_d(fmt, ...) ((void)0)
