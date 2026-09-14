// Boot-time hardware probe. Logs "[probe] ..." lines over serial so the board
// variant (panel size, resistive vs capacitive touch) can be identified from
// the console alone, without seeing the screen. DESIGN.md SS3, docs/hardware.md.
#pragma once

namespace transit_app {

// Call BEFORE smartdisplay_init(): scans the two candidate touch I2C pin pairs
// (SDA 33 / SCL 32 as wired on the 3.5" and 2.4" capacitive boards, then
// SDA 21 / SCL 22 as some references claim) and releases the pins afterwards.
// Also logs chip, flash, heap, and MAC.
void hwProbeEarly();

// Call AFTER smartdisplay_init(): logs the LVGL display resolution and rotation.
void hwProbeDisplay();

}  // namespace transit_app
