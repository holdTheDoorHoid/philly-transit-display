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

## `DISPLAY_INVERT_DEFAULT`

Project-specific flag (not from upstream): the boot default for `device.invert_colors`, the
panel colour-inversion setting. No vendored board sets it (absent means 0); the owner's 3.5"
panel turned out not to need inversion - see `docs/hardware.md` "Colour inversion" for the
LVGL background-opacity mistake that made it look as if it did. Set it to 1 in a board file
only for a variant verified to render inverted; users can override it at runtime from Settings.

## `LV_FONT_MONTSERRAT_20`

The 240-tall boards (`esp32-2432S028R`, `esp32-2432S028Rv3`, `esp32-2432S024R`,
`esp32-2432S024C`) pass `-D LV_FONT_MONTSERRAT_20=1` because `ui_common.cpp`'s `fontBig()` uses
size 20 for the big minutes on those boards, in both orientations; the 3.5" boards use 28. The
choice is made at compile time so each board links only one big font: `lv_conf.h` defaults 20 to
0, which keeps ~22 KB out of the 3.5" build, and the small boards drop the ~32 KB Montserrat 28
(firmware/README.md "Memory and flash budget").

## `LVGL_BUFFER_PIXELS` on the 3.5" boards

`esp32-3248S035R.json` and `esp32-3248S035C.json` size the draw buffer at **1/40** of the screen
(3,840 px, 7,680 B): `/16` first became `/20` (7,680 px, 15 KB) when the 2026-09-14 features needed
the 4 KB back, `/30` in 0.3.1 for the same reason again, and `/40` in 0.3.2-rc3 - it is a
permanent block of the ESP32's byte-addressable heap, allocated once at `smartdisplay_init()` and
held for the life of the device, on a board whose largest free block is the resource that runs out
(DESIGN.md SS12.1). `ST7796_SPI_BUS_MAX_TRANSFER_SZ` is written in terms of `LVGL_BUFFER_PIXELS`,
so the SPI DMA descriptor count follows it automatically and needs no separate edit.

The `/30` -> `/40` step is 2,560 B and the owner delegated the size to our judgement. It is the
cheapest remaining block on the board that costs no functionality at all: the buffer is a
scratchpad the panel driver flushes from, so a smaller one changes how often a repaint is flushed
and nothing about what is drawn. 3,840 px is 12 rows of a 320-wide panel, comfortably more than
the one row LVGL requires.

The cost is flush count: 40 partial flushes per full repaint instead of 30, at an unchanged
24 MHz pixel clock. The observable is `tick_ms_max` in `GET /api/debug/ui` (21 ms on the owner's
board at `/20`, 168 ms across a full device-suite run at `/30` with a page rebuild as the
expensive case) - **re-measure it after flashing rather than assuming**, because this is the third
consecutive release to shrink this buffer and each one lengthens a repaint. The 240-tall boards
stay at `/16`: their quarter is 9.6 KB to begin with, and they are not the board this was measured
on.

## `LV_MEM_SIZE` stays at 36 KB (0.3.1)

Every KB of LVGL's static pool (`firmware/include/lv_conf.h`) is a KB the heap never gets, so
shrinking it to 28 KB was the largest single item in the 2026-09-17 RAM audit. It is **not** being
taken, and the reason is the arrivals page: the owner wants up to four stops on it, and four stops
is already at the edge at 36 KB.

Measured with the host pool sweep, which scales its pool to board-equivalent bytes
(`pio run -e ui-sim-pool && .pio/build/ui-sim-pool/program /tmp/x pool`, `kHostToBoard` in
`sim/sim_main.cpp`), on the 320x480 panel:

| board-equivalent pool | 4-stop arrivals page | pool free with it up | panels left off |
|---:|---:|---:|---:|
| 37,847 B (the sweep's own 56 KB build, ~1 KB more than a board) | 31,638 B | 2,640 B | none |
| 36,864 B (the real pool) | 25,687 B | 9,392 B | **1** |
| 34,816 B (the proposed 34 KB) | 25,714 B | 6,248 B | **1** |

Read the last two rows carefully: the page gets *cheaper* because `main_screen.cpp`'s panel guard
is leaving a panel OFF - a stop the owner configured does not appear. So 34 KB does not buy a
tighter fit, it buys a missing panel, and it is 2 KB further from the four-stop case rather than
nearer. 28 KB is further still.

The margin at 36 KB is thin enough to be worth saying out loud: a four-stop page sits within about
a kilobyte of the guard on this arithmetic, and `kHostToBoard` itself is a 0.66 fit with a 1.5 %
spread. `GET /api/debug/ui`'s `lv_free`, `lv_page_cost` and `lv_page_refusals` are what settle it on
the hardware; `lv_tight` goes true when the page that is up left under ~3 KB.

What did change is `LV_DRAW_LAYER_SIMPLE_BUF_SIZE`, 24 KB -> 8 KB (`lv_conf.h`): that is the size
LVGL asks this same 36 KB pool for when it buffers a widget into a simple layer, and 24 KB out of a
pool with single-digit kilobytes free could never be granted. It costs no static RAM either way.
