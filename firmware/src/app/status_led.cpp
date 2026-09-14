#include "status_led.h"

#include <Arduino.h>
#include <esp32_smartdisplay.h>

namespace transit_app {

namespace {
LedState g_state = LedState::Off;

void applyState(LedState state) {
#ifdef BOARD_HAS_RGB_LED
  switch (state) {
    case LedState::Off:
      smartdisplay_led_set_rgb(false, false, false);
      break;
    case LedState::Connecting:
      smartdisplay_led_set_rgb(false, false, true);  // blue
      break;
    case LedState::Error:
      smartdisplay_led_set_rgb(true, false, false);  // red
      break;
  }
#else
  (void)state;
#endif
}
}  // namespace

void setStatusLed(LedState state) {
  g_state = state;
  applyState(state);
}

void flashPollOk(uint32_t on_ms) {
#ifdef BOARD_HAS_RGB_LED
  smartdisplay_led_set_rgb(false, true, false);  // green
  delay(on_ms);
  applyState(g_state);
#else
  (void)on_ms;
#endif
}

}  // namespace transit_app
