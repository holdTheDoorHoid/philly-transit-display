# Hardware: pin maps, flashing, and adding a board variant

This is the firmware-skeleton counterpart to `docs/research/cyd-hardware.md` (that
document is broader background research; this one is the operational reference for
the boards this firmware actually targets). Pin numbers below come directly from the
vendored board JSONs in `firmware/boards/` (source: `rzeldent/platformio-espressif32-sunton`,
commit `0d9a9b1a4`, downloaded 2026-09-13 - see `firmware/boards/README.md`), not from
aggregator sites, and are what `rzeldent/esp32_smartdisplay` actually wires up when you
build one of this project's `cyd-*` PlatformIO environments.

> **Correction to earlier project research:** `docs/research/cyd-hardware.md` guessed the
> 3.5" capacitive board's GT911 touch controller sits on I2C SDA=21/SCL=22. The board JSON
> says SDA=**33**, SCL=**32** (plus separate RST=25 and INT=21 lines) - see the table below.
> That research doc was written from secondary sources before this JSON was vendored; this
> file is the one to trust for pin numbers.

## 3.5" boards: `cyd-3248S035R` and `cyd-3248S035C`

Both are 320x480 ST7796 panels (portrait by default, `device.rotation` picks the orientation
via `lv_display_set_rotation()` in `main.cpp`); they differ only in the touch controller.

### Colour inversion

`esp32_smartdisplay` sends the ST7796's colour-inversion command only when a board file defines
`DISPLAY_IPS`, and none of the vendored Sunton files do. The owner's `ESP32-3248S035R` has an IPS
panel: without inversion its near-black UI background rendered as white, and the panel flashed
white (not black) while the firmware was crash-looping (2026-09-14). So the 3.5" board files carry
`-D DISPLAY_INVERT_DEFAULT=1`, which seeds the runtime setting `device.invert_colors`
(`ui::applyInvert()`, Settings > "Invert panel colors"). If a unit shows the light theme as dark,
an orange route badge instead of blue, or a white flash at boot, flip that setting - no rebuild
needed. The 2.4"/2.8" board files leave the default off, matching upstream.

### TFT panel (ST7796, SPI2_HOST) - same on both variants

| Signal | GPIO |
|---|---|
| MOSI | 13 |
| MISO | 12 |
| SCLK | 14 |
| CS | 15 |
| DC (D/C) | 2 |
| RST | not wired (`GPIO_NUM_NC`) - panel reset is done in software |
| Backlight | 27 |

### Touch - resistive (`cyd-3248S035R`): XPT2046, shares the TFT's SPI2_HOST bus

| Signal | GPIO |
|---|---|
| MOSI / MISO / SCLK | 13 / 12 / 14 (same physical bus as the TFT, different CS) |
| CS | 33 |
| IRQ | 36 |

### Touch - capacitive (`cyd-3248S035C`): GT911, I2C_NUM_0 @ 400 kHz

| Signal | GPIO |
|---|---|
| SDA | 33 |
| SCL | 32 |
| RST | 25 |
| INT | 21 |
| I2C address | `0x5D` (the board JSON sets `GT911_IO_I2C_CONFIG_DEV_ADDR` to the `_5D` variant; GT911's other possible address is `0x14`) |

### Shared peripherals (both 3.5" variants)

| Peripheral | Signal | GPIO |
|---|---|---|
| microSD (own SPI bus, separate from the TFT) | CS | 5 |
| | MOSI | 23 |
| | MISO | 19 |
| | SCK | 18 |
| RGB status LED (active low, per DESIGN.md SS3) | Red | 4 |
| | Green | 16 |
| | Blue | 17 |
| LDR (ambient light, analog) | - | 34 |
| Speaker (via onboard FM8002A amp) | - | 26 |

`firmware/src/app/sd_logger.cpp` opens the SD card on its own `SPIClass(VSPI)` instance
(the TFT/touch bus above uses `SPI2_HOST`/HSPI) so SD traffic never contends with display
writes. `firmware/src/app/status_led.cpp` and the RGB pins are wrapped by
`esp32_smartdisplay`'s `smartdisplay_led_set_rgb()`, which already accounts for the
active-low wiring.

## Telling the R and C variants apart

1. **Silkscreen** - printed model string ends in `R` (resistive) or `C` (capacitive); fastest
   check when legible.
2. **Physical** - a resistive touch panel has a slightly "give" plastic-film overlay; a
   capacitive one is rigid glass, and its FPC connector is usually stamped `GT911` (3.5"/2.4"
   boards in this project) or `CST816S` (the 2.4" `C` board specifically - see below).
3. **Software/electrical** - probe I2C for an ACK at `0x5D`/`0x14` (GT911) or `0x15`
   (CST816S/CST820-family controllers): an ACK means capacitive; if instead the XPT2046 IRQ
   line (GPIO 36 on every board in this project) responds to a touch, it's resistive. This is
   the reliable fallback when the silkscreen is worn off, since batch variance is common in
   this hardware family (see `docs/research/cyd-hardware.md` SS1).

**DESIGN.md correction:** DESIGN.md SS3's table lists the 2.4" capacitive board's touch
controller as "CST820". The vendored board JSON (`firmware/boards/esp32-2432S024C.json`,
from the same upstream repo) actually configures **CST816S**, not CST820 - see
`firmware/boards/README.md` for the full note. If a real CST820 unit turns up, it would need
its own board JSON/entry; nothing in this repo currently drives one.

## Other vendored variants (brief reference)

For the 2.8"/2.4" boards, only what differs from the 3.5" table above:

| env | Panel driver | Backlight | Touch | Notes |
|---|---|---|---|---|
| `cyd-2432S028R` | ILI9341 | GPIO 21 (not 27!) | XPT2046, CS 33 / IRQ 36 | Single micro-USB; TFT/touch bus pins otherwise match the 3.5" table (13/12/14) |
| `cyd-2432S028Rv3` | ST7789 | GPIO 21 | XPT2046, CS 33 / IRQ 36, **X-mirrored** | 2-USB board; `DISPLAY_MIRROR_X=false` but `TOUCH_MIRROR_X=true` - display and touch handedness are configured independently in the board JSON; MISO is unused (`GPIO_NUM_NC`) since ST7789 here is write-only |
| `cyd-2432S024R` | ILI9341 | GPIO 27 | XPT2046, CS 33 / IRQ 36 | |
| `cyd-2432S024C` | ILI9341 | GPIO 27 | **CST816S** (I2C_NUM_0, SDA 33 / SCL 32 / RST 25 / INT 21) | Not CST820 - see correction above |

SD (CS 5 / MOSI 23 / MISO 19 / SCK 18), RGB LED (4/16/17), LDR (34), and speaker (26) are
identical across every board in this project.

## Backlight / brightness control

`config.device.brightness` (0-100, DESIGN.md SS6) is applied via `esp32_smartdisplay`'s own
`smartdisplay_lcd_set_backlight(float duty)` (`include/esp32_smartdisplay.h`; found by reading
the vendored library source under `firmware/.pio/libdeps/<env>/esp32_smartdisplay/`, since
neither the library's README nor DESIGN.md names the exact function) - `duty` is `[0, 1]`, so
`firmware/src/app/ui/ui.cpp`'s `applyBrightness(uint8_t percent)` just divides by 100. The
library already owns the LEDC channel it wires to each board's `DISPLAY_BCKL` pin (27 on the
3.5" boards and the 2.4" `R`/`C` boards; 21 on the 2.8" boards - see the tables above), so
nothing in `firmware/src/app/` touches LEDC or that GPIO directly; doing so independently would
double-attach the pin's LEDC channel.

## Flashing

`pio run -e <env> -t upload --upload-port /dev/ttyUSB0` is the normal path - PlatformIO
drives `esptool.py` itself using the addresses below, taken from this project's partition
table (`firmware/partitions.csv`; see that file's own header comment, and
`firmware/platformio.ini`'s comments, for why a custom table was needed instead of one of
Arduino-ESP32's built-in schemes - in short, this firmware doesn't fit in the standard
`default.csv` 1.25 MiB OTA app slots).

Equivalent manual command, if you already have `bootloader.bin`/`partitions.bin`/`firmware.bin`
(e.g. from a release artifact rather than a local build):

```sh
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash \
  0x1000  bootloader.bin \
  0x8000  partitions.bin \
  0x10000 firmware.bin
```

If `--baud 921600` produces flashing errors on a particular USB-serial adapter, step down to
`460800` and then `115200`.

**Linux `dialout` group:** `/dev/ttyUSB0` (the CH340 USB-serial chip common to this whole
board family) is owned by `root:dialout` by default. Add your user once with
`sudo usermod -aG dialout $USER`, then log out and back in - the common one-off alternative,
`sudo chmod a+rw /dev/ttyUSB0`, doesn't survive a replug or reboot.

## Adding another Sunton variant

1. Find the board's JSON filename in `rzeldent/platformio-espressif32-sunton` (the table in
   that repo's README maps model numbers to filenames - see `firmware/boards/README.md` for
   the exact commit this project vendors from).
2. Download it into `firmware/boards/`, e.g.:
   ```sh
   curl -sL -o firmware/boards/esp32-<name>.json \
     https://raw.githubusercontent.com/rzeldent/platformio-espressif32-sunton/<commit>/esp32-<name>.json
   ```
3. Add an environment to `firmware/platformio.ini`:
   ```ini
   [env:cyd-<name>]
   extends = esp32_base
   board = esp32-<name>
   ```
4. Update `firmware/boards/README.md`'s table (and the commit/date if you refreshed from a
   newer upstream commit while you were there).
5. No UI code changes should be needed for a panel of a different size: `ui_common.h`'s
   `screenSize()`, `rowsPerStop()`, and `font*()` all read the runtime resolution
   (`lv_display_get_horizontal/vertical_resolution()`), per DESIGN.md SS8. A panel with an
   unusual touch controller, orientation quirk, or missing peripheral (no SD, no RGB LED) may
   need the `#ifdef BOARD_HAS_*` guards in `firmware/src/app/` extended - grep for
   `BOARD_HAS_TF`/`BOARD_HAS_RGB_LED` for the existing pattern.
6. Build it: `pio run -e cyd-<name>`.
