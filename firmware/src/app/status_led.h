// Onboard RGB LED status indicator. DESIGN.md SS3: "RGB LED pins 4/16/17
// (active low) can show status: blue = connecting, green blink = poll ok,
// red = error."
#pragma once
#include <cstdint>

namespace transit_app {

enum class LedState {
  Off,
  Connecting,  // blue: Wi-Fi/portal in progress
  Error,       // red: last poll or subsystem init failed
};

// Sets the steady-state LED color. Only takes effect on boards whose board
// JSON defines BOARD_HAS_RGB_LED (all six boards in firmware/boards/ do);
// a no-op otherwise. Uses esp32_smartdisplay's smartdisplay_led_set_rgb(),
// which already accounts for the LED's active-low wiring - `true` here
// always means "lit", regardless of polarity.
void setStatusLed(LedState state);

// Briefly flashes the LED green to indicate a successful poll, then
// restores whatever setStatusLed() was last given. Blocking (uses delay());
// only call this from a task where a short pause is harmless, such as
// net_poller's own task - never from the LVGL task/loop().
void flashPollOk(uint32_t on_ms = 150);

}  // namespace transit_app
