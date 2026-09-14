# Vendored Sunton board definitions

The JSON files in this directory are PlatformIO board definitions for the Sunton
ESP32 "Cheap Yellow Display" (CYD) family, consumed by `rzeldent/esp32_smartdisplay`
(see `firmware/platformio.ini`).

- Source repo: <https://github.com/rzeldent/platformio-espressif32-sunton>
- Commit vendored: `d8a92707f0af254daa71162cdd9eb40e38962286` (2025-06-12)
- Downloaded: 2026-09-13, via `curl` from `raw.githubusercontent.com` (one file per
  board), not as a git submodule - this project vendors dependencies rather than
  using submodules.
- License: see the upstream repo (no separate LICENSE file was vendored here;
  these are plain data/config JSON, not source code).

**Why this specific commit, not the latest one:** the first vendoring pass here
used the repo's then-current `main` (`0d9a9b1a4`, 2026-06-02), which built but
crashed a step further at compile time with undeclared macros like
`ST7796_SPI_CONFIG_FLAGS_DC_AS_CMD_PHASE` in `esp32_smartdisplay`'s panel-init
code. Cause: `rzeldent/esp32-smartdisplay`'s own repo pins `boards/` as a git
**submodule** at a specific commit (checked via GitHub's contents API on the
`2.1.1` tag) - `esp32_smartdisplay` and this board-JSON repo are meant to be
consumed as a matched pair, and a board-JSON commit a year newer than the
`esp32_smartdisplay` release it's paired with can drift in exactly this way
(new flag names the older library doesn't know about, or vice versa). This
project now vendors the same commit `esp32_smartdisplay@2.1.1` itself points
at, not whatever is newest upstream. See `firmware/platformio.ini`'s
build-notes comment and `firmware/extra_scripts/patch_esp32_smartdisplay.py`
for the (separate) compile-time bugs in `esp32_smartdisplay@2.1.1` itself that
this fixed too.

## Files

| File | Board | Panel | Touch |
|---|---|---|---|
| `esp32-3248S035R.json` | ESP32-3248S035R | 3.5" ST7796 320x480 | XPT2046 resistive (SPI) |
| `esp32-3248S035C.json` | ESP32-3248S035C | 3.5" ST7796 320x480 | GT911 capacitive (I2C) |
| `esp32-2432S028R.json` | ESP32-2432S028R | 2.8" ILI9341 240x320, single micro-USB | XPT2046 resistive (SPI) |
| `esp32-2432S028Rv3.json` | 2-USB 2.8" variant | ST7789 240x320 | XPT2046 resistive (SPI), X-mirrored |
| `esp32-2432S024R.json` | ESP32-2432S024R | 2.4" ILI9341 240x320 | XPT2046 resistive (SPI) |
| `esp32-2432S024C.json` | ESP32-2432S024C | 2.4" ILI9341 240x320 | CST816S capacitive (I2C) - see note below |

Note the JSONs describe panels in native (portrait) orientation
(`DISPLAY_WIDTH`/`DISPLAY_HEIGHT`); the UI runs in landscape via
`lv_display_set_rotation()` at startup (see `firmware/src/main.cpp`).

**DESIGN.md correction:** DESIGN.md SS3 lists the 2432S024C's capacitive touch
controller as "CST820". The actual vendored board JSON (and the upstream
`platformio-espressif32-sunton` repo) uses **CST816S**, not CST820. CST820 does
not appear anywhere in the upstream repo's board list. Pin maps and driver
selection in this project follow the JSON (CST816S), not DESIGN.md's guess.
Similarly, `docs/research/cyd-hardware.md` SS "ESP32-3248S035(R/C)" guessed the
GT911 touch controller sits on I2C SDA=21/SCL=22; the vendored board JSON says
SDA=33, SCL=32 (with separate RST=25, INT=21 lines) - see `docs/hardware.md`
for the corrected pin map sourced directly from the JSON.

## How this list was chosen

DESIGN.md SS3 asks for 3.5" R/C, 2.8" single-USB R, 2.8" 2-USB variant, and 2.4"
R/C. The upstream repo's board list was fetched (`git/trees` API) to get the
exact filenames - in particular, to confirm the 2-USB/ST7789 2.8" variant that
DESIGN.md calls "esp32-2432S028Rv3 (or whatever the 2-USB variant is called)"
is in fact named `esp32-2432S028Rv3.json` (there is also a `esp32-2432S028Rv2.json`,
an earlier revision not requested by DESIGN.md and not vendored here).

## Updating

To refresh a board definition or add a new Sunton variant, see
`docs/hardware.md` ("Adding another Sunton variant").

## Local modification: LVGL draw buffer size

All six JSONs have `LVGL_BUFFER_PIXELS` changed from `DISPLAY_WIDTH*DISPLAY_HEIGHT/4` to
`/16`. With Arduino core 3.x the largest free heap block on these PSRAM-less boards is
about 110 KB at boot; a quarter-screen RGB565 buffer on the 320x480 panels is 76.8 KB
(and the upstream driver mistakenly sized it with the 3-byte `lv_color_t`, 115 KB, which
failed outright). A sixteenth of the screen (19.2 KB on 3.5", 9.6 KB on 2.8") is LVGL's
recommended partial-render minimum and leaves room for Wi-Fi, TLS, and the web server.
See `firmware/extra_scripts/patch_esp32_smartdisplay.py` for the `lv_color16_t` fix.
