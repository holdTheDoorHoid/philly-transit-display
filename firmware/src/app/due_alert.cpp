#include "due_alert.h"

#include <Arduino.h>

#include "status_led.h"

namespace transit_app {

namespace {

// Trips that already got their beeps; a small ring so a bus that hovers around the threshold
// (or a re-poll that re-orders rows) doesn't chime twice. 16 is far more than the rows on screen.
constexpr size_t kChimedRing = 16;
std::string g_chimed[kChimedRing];
size_t g_chimed_next = 0;
bool g_led_phase = false;
bool g_led_overriding = false;

bool alreadyChimed(const std::string &trip) {
  for (const std::string &t : g_chimed) {
    if (t == trip) return true;
  }
  return false;
}

void rememberChimed(const std::string &trip) {
  g_chimed[g_chimed_next] = trip;
  g_chimed_next = (g_chimed_next + 1) % kChimedRing;
}

void chime() {
#if defined(BOARD_HAS_SPEAK) && defined(SPEAK)
  // Two short beeps through the board's amp (GPIO 26 on the Sunton boards). tone() is
  // non-blocking on the ESP32 core; the 160 ms wait between them runs on the LVGL task once per
  // newly-due trip, which is rare enough not to matter.
  tone(SPEAK, 880, 120);
  delay(160);
  tone(SPEAK, 1175, 120);
#endif
}

}  // namespace

bool arrivalIsDue(const Config &cfg, const transit::Arrival &a, transit::Epoch now) {
  if (!cfg.due.enabled || a.status == transit::Status::Skipped) return false;
  transit::Epoch eff = a.effective();
  if (eff <= 0) return false;
  transit::Epoch eta = eff - now;
  return eta >= -60 && eta <= (transit::Epoch)cfg.due.minutes * 60;
}

bool dueAlertTick(const Config &cfg, const transit::Snapshot &snap, const std::vector<std::string> &shown_keys,
                  bool quiet, transit::Epoch now) {
  bool any_due = false;
  if (cfg.due.enabled) {
    for (const transit::StopSnapshot &stop : snap.stops) {
      bool shown = false;
      for (const std::string &k : shown_keys) {
        if (k == stop.key) {
          shown = true;
          break;
        }
      }
      if (!shown) continue;
      for (const transit::Arrival &a : stop.arrivals) {
        if (!arrivalIsDue(cfg, a, now)) continue;
        any_due = true;
        // Only a live prediction earns a beep: a schedule-only row is a guess, not a bus.
        if (cfg.due.chime && !quiet && a.status == transit::Status::Live && !a.trip.empty() && !alreadyChimed(a.trip)) {
          rememberChimed(a.trip);
          chime();
        }
      }
    }
  }

  if (any_due && cfg.due.led) {
    g_led_phase = !g_led_phase;
    ledOverrideGreen(g_led_phase);
    g_led_overriding = true;
  } else if (g_led_overriding) {
    ledOverrideGreen(false);  // restores the status colour
    g_led_overriding = false;
    g_led_phase = false;
  }
  return any_due;
}

}  // namespace transit_app
