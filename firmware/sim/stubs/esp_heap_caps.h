// Host stand-in for ESP-IDF's <esp_heap_caps.h>, for the screenshot simulator (firmware/sim).
// Only what the LVGL screens in src/app/ui/ actually touch: the device page prints the
// byte-addressable free heap rather than ESP.getFreeHeap(), because on the real chip the latter
// also counts ~34 KB of 32-bit-word-only IRAM that malloc() never hands out for data (DESIGN.md
// §2.1). The simulator has one flat notion of "free", so both capabilities answer from
// sim::free_heap and the page renders the same on the host as on the device.
//
// Like the other stubs here, -Isim/stubs comes first on the include path so this shadows the real
// header on the host build only; the ESP32 environments never see this directory.
#pragma once
#include <cstddef>
#include <cstdint>

#include <Arduino.h>  // sim::free_heap

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)

inline size_t heap_caps_get_free_size(uint32_t) { return sim::free_heap; }
// The simulator does not model fragmentation; the device page only reads the free size, and a
// caller that wants a largest block on the host would be measuring the stub, not the firmware.
inline size_t heap_caps_get_largest_free_block(uint32_t) { return sim::free_heap; }
