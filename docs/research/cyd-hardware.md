# ESP32 "Cheap Yellow Display" (CYD) research — transit-arrivals desk display

Researched 2026-09-13. Primary sources: witnessmenow/ESP32-Cheap-Yellow-Display GitHub repo,
rzeldent/platformio-espressif32-sunton board defs, Random Nerd Tutorials, Mischianti, LVGL docs,
Espressif docs, esp-rs docs/GitHub, ESP32 forum. Secondary/aggregator sources (esp32s.com,
ElectricalFlux, espboards.dev) used where primary sources were thin — flagged as such.
Anything I couldn't confirm from a primary source is marked **[UNVERIFIED]**.

---

## 1. CYD variants and how to tell them apart

The "CYD" name covers a family of cheap Sunton-made ESP32 dev boards with an integrated
resistive/capacitive touch TFT, sold under many AliExpress storefronts with no consistent
naming. The witnessmenow repo is the closest thing to a canonical reference, but even it
notes hardware varies batch-to-batch. Model numbers follow a rough pattern
`ESP32-<size><resolution><variant><touch>` (e.g. `2432` = 240×320, `3248` = 320×480).

### ESP32-2432S028R — 2.8", single micro-USB, resistive (the "original" CYD)
- **Display controller:** ILI9341 (SPI/HSPI).
- **Touch controller:** XPT2046 resistive, on its own SPI bus.
- **Resolution:** 240×320 (physical 2.8").
- **MCU/memory:** ESP32-WROOM-32 (dual-core Xtensa LX6 @ 240 MHz), **4 MB flash, no PSRAM**,
  520 KB total SRAM (~320 KB usable as DRAM before Wi-Fi/TLS eat into it — see §4).
- **Gotchas:** Backlight is a plain GPIO (21), not tied to a hardware PWM channel by default —
  you attach it to an LEDC channel yourself if you want dimming. TFT reset line is not wired
  to a GPIO (RST = -1 in most configs); reset happens via SPI software command or shares the
  chip EN line. Touch and display are on **separate hardware SPI buses** (a common config
  mistake — many generic ILI9341/XPT2046 example sketches assume one shared bus).
- **How to identify:** single micro-USB port on the edge; silkscreen usually reads
  "ESP32-2432S028R"; software-wise, ILI9341's `RDDID`/`RDDST` (0x04/0x09) register reads
  return the ILI9341 signature, and `TFT_eSPI`'s `ILI9341_2` driver variant works.
  ([witnessmenow issue #278](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/issues/278))

### The two-USB-port 2.8" variant (often labeled "V2"/"V3", CYD2USB) — ST7789, not ILI9341
- **Display controller:** despite often being driven with an "ILI9341_2" config, the
  2-USB-port board actually has an **ST7789** panel. Community reports (Bodmer/TFT_eSPI
  discussion #3018, r4jrk/cyd2usb sample repo) confirm this directly — driving it with
  ILI9341 settings gives washed-out/"milky" gamma.
- **Touch controller:** still XPT2046 resistive, but wiring/orientation on some batches needs
  an axis swap/mirror (`rzeldent`'s `esp32-2432S028Rv3.json` sets **X-axis mirroring** in its
  touch config, IRQ still GPIO 36, SPI3_HOST @ 2 MHz, Z-threshold 600).
- **Known TFT_eSPI fix for this variant:**
  ```
  ST7789_DRIVER
  TFT_INVERSION_OFF        // NOT INVERSION_ON like the ILI9341 board
  TFT_RGB_ORDER  TFT_BGR
  ```
  (single-USB ILI9341 board instead wants `TFT_INVERSION_ON`.) Source:
  [Bodmer/TFT_eSPI #3018](https://github.com/Bodmer/TFT_eSPI/discussions/3018).
- **USB-C caveat:** the added USB-C port on these boards commonly **lacks CC resistors**, so
  USB-C-to-C cables won't power it — you still need the micro-USB or a C-to-A cable.
  **[secondary source — atomic14.com, not independently verified]**
- **How to identify:** two USB connectors (micro-USB + USB-C) on the edge is the fast tell —
  "single USB = ILI9341, dual USB = ST7789" is the community rule of thumb, though it is a
  correlation, not a guarantee, given batch variance. Software-side identification (safer):
  read the panel ID register before committing to a driver — ILI9341's `RDDST` (0x09) must be
  read with dummy-clock/sequential timing, ST7789's ID4 (0xD3) is read indexed; a small probe
  library exists for this: [ropg/spi_lcd_read](https://github.com/ropg/spi_lcd_read). There is
  also a purpose-built variant-detector/diagnostic sketch:
  [HenrysCat/ESP32-2432S028-CYD-Diagnostic-Tool](https://github.com/HenrysCat/ESP32-2432S028-CYD-Diagnostic-Tool).

### ESP32-2432S024(R/C) — 2.4"
- **Display controller:** IPS LCD panel, 240×320 (same resolution as the 2.8", just a smaller
  physical panel, rated ~300 nits vs. the TN 2.8" panel's dimmer output).
  **[secondary source — espboards.dev / esp32s.com]**
- **Touch controller:** resistive XPT2046 on the "R" suffix; the **capacitive "C" suffix
  variant swaps XPT2046 for a CST820** controller (I2C, not SPI).
- **Gotcha:** touch CS pin and backlight pin reportedly differ from the 2.8" R board on some
  batches — don't assume the 2432S028R pin map applies unmodified.
  **[secondary source, unverified against a primary schematic]**
- **How to identify:** physically smaller screen; check silkscreen for "S024"; for R vs C,
  physical press with a stylus (works on resistive, may not register on capacitive) or probe
  the touch IC over I2C (CST820 has a fixed I2C address) vs SPI (XPT2046).

### ESP32-3248S035(R/C) — 3.5", ST7796
- **Display controller:** ST7796, 320×480, driven over the same HSPI-style bus pattern as the
  2.8" board.
- **Touch controller:** "R" = resistive XPT2046; **"C" = capacitive GT911** (I2C).
- **Pin map (per espboards.dev / esp32pins.com, cross-checked, not from an official
  schematic):**
  - TFT: MISO 12, MOSI 13, SCLK 14, CS 15, DC 2, RST unused (-1), Backlight 27 (note: **not**
    21 as on the 2.8" board).
  - GT911 touch (capacitive "C" variant): I2C on **GPIO 21 (SDA) / GPIO 22 (SCL)** — shared
    I2C bus, not SPI, and note GPIO 21 here is a *different* function than the 2.8" board's use
    of GPIO 21 for backlight.
  - RGB LED: R=4, G=16, B=17 (same as 2.8" board).
  - Onboard audio amp (FM8002A) driving the speaker connector.
  - Recommended free GPIOs for expansion: 25, 26 (per this source).
- **Gotcha:** because backlight and touch controller assignments shift between the 3.5" and
  2.8" boards, **pin maps are not interchangeable across CYD sizes** — always confirm against
  the specific board's silkscreen/model number, not just "CYD."

### General identification strategy
1. **Silkscreen first** — the model string (e.g. "ESP32-2432S028R", "ESP32-3248S035C") is
   printed on the PCB and is the fastest, most reliable signal when legible.
2. **Physical**: count USB ports (1 vs 2), screen size, and touch feel (glassy flex under
   light pressure = resistive; rigid cover glass = capacitive, usually stamped "GT911" or
   "CST820" near the FPC connector).
3. **Software (most reliable when silkscreen is worn/missing)**: read the display controller's
   ID registers over SPI before initializing a specific driver, and probe the I2C bus for
   GT911 (default addr `0x5D` or `0x14`) / CST820 (`0x15`) — presence of an ACK on I2C at those
   addresses means capacitive touch; absence + a working XPT2046 on the touch SPI bus means
   resistive.
4. On this project's actual hardware — appears on Linux as CH340 (`1a86:7523`) — that only
   confirms the **USB-serial chip**, common to nearly every CYD variant (including the 2-USB
   board, which typically pairs CH340 on the micro-USB side with a CP2102 or another CH34x on
   USB-C, or vice versa depending on batch). It does **not** tell you the display/touch
   variant; probe as above once flashed.

---

## 2. Full pin map — ESP32-2432S028R (2.8", single micro-USB, ILI9341/XPT2046)

Cross-checked across Random Nerd Tutorials' CYD pinout page and Mischianti's CYD pinout page;
the two agree on every pin below.

### TFT display (ILI9341) — HSPI
| Function | GPIO |
|---|---|
| MOSI | 13 |
| MISO | 12 |
| SCLK | 14 |
| CS | 15 |
| DC (D/C) | 2 |
| RST | not wired to a GPIO (-1; driven by software reset command) |
| Backlight (BL) | 21 (plain digital; wire to an LEDC PWM channel for dimming) |

### Touchscreen (XPT2046, resistive) — own SPI bus (VSPI, separate from TFT's HSPI)
| Function | GPIO |
|---|---|
| MOSI (T_DIN) | 32 |
| MISO (T_DO) | 39 (input-only pin) |
| SCK (T_CLK) | 25 |
| CS (T_CS) | 33 |
| IRQ (T_IRQ) | 36 (input-only pin) |

### microSD card — shares the touch's SPI bus (VSPI) electrically, separate CS
| Function | GPIO |
|---|---|
| MOSI | 23 |
| MISO | 19 |
| SCK | 18 |
| CS | 5 |

**Shared-bus constraint:** the SD card and the XPT2046 touch controller are wired to the
*same* VSPI hardware SPI peripheral but with distinct CS lines (SD CS=5, touch CS=33) — they
cannot be read simultaneously (SPI bus is inherently half-duplex-shared), but the driver stack
can multiplex them by CS the same way you'd handle two chip-selects on any shared bus, at the
cost of clock-speed mismatches (XPT2046 wants ≤2.5 MHz, SD cards typically want 20–40 MHz) —
practically this means dropping to the touch controller's low clock whenever you talk to it,
then ramping back up for SD transfers. **The TFT is on a completely separate SPI peripheral
(HSPI)**, so display writes never contend with SD/touch traffic. This is the single most
commonly misunderstood point in CYD wiring guides — several forum threads exist purely because
people assumed one shared "SPI bus" for everything.

### RGB LED (discrete, active-LOW)
| Color | GPIO |
|---|---|
| Red | 4 |
| Green | 16 |
| Blue | 17 |

### Other onboard peripherals
| Function | GPIO |
|---|---|
| LDR (ambient light, analog) | 34 (ADC1-capable, input-only) |
| Speaker / audio out (2-pin JST) | 26 |
| BOOT button | 0 |
| Reset button | wired to EN, no GPIO needed |

### Expansion connectors
| Connector | Pins |
|---|---|
| **CN1** (4-pin, often silkscreened for I²C) | GND — GPIO 22 (SCL) — GPIO 27 (SDA) — 3.3V |
| **P3** (4-pin) | GND — GPIO 35 (input-only) — GPIO 22 — GPIO 21 (yes, backlight is also broken out here) |
| **P1** (UART, 4-pin) | VIN — TX (GPIO1) — RX (GPIO3) — GND. **Also the USB-CH340 UART** — using it for anything else conflicts with USB/serial-console use and has known routing quirks; treat as effectively unavailable for a second UART peripheral while USB is plugged in. |

### Which pins are actually free
After display, touch, SD, RGB LED, LDR, and speaker are accounted for, the **only genuinely
free GPIOs are 22, 27, and 35 (input-only)**. GPIO 22 and 27 are both duplicated onto CN1 as a
pseudo-I²C header, so if this project wants an external I2C sensor (e.g. a proper light/temp
sensor to complement the crude LDR), CN1 is the natural spot — but note GPIO 22 is *also*
broken out on P3, so don't drive it from two different peripherals at once. GPIO 21
(backlight) is technically available on P3 too, but it's already committed to backlight duty.
There is **no free hardware UART, no free ADC2 channel** (ADC2 is unusable anyway when Wi-Fi
is active, a classic ESP32 classic gotcha — see §4), and no free hardware SPI/I2S peripheral
without stealing a bus already used by touch/SD.

**Practical implication for this project:** with only GPIO 22/27/35 free, there is no room to
add e.g. a rotary encoder *and* a status LED *and* an external sensor without going through an
I2C GPIO expander (PCF8574/MCP23017) on CN1, or dropping one of the onboard features (e.g.
repurposing the RGB LED pins if the RGB LED isn't needed) to reclaim GPIO 4/16/17.

Sources:
[Random Nerd Tutorials CYD pinout](https://randomnerdtutorials.com/esp32-cheap-yellow-display-cyd-pinout-esp32-2432s028r/),
[Mischianti CYD pinout](https://mischianti.org/esp32-2432s028-cheap-yellow-display-high-resolution-pinout-datasheet-schema-and-specs/).

---

## 3. Firmware stack options

### (a) PlatformIO + Arduino framework + LVGL 9 + a display driver library

**Which display library does the CYD community actually use in 2026?** LovyanGFX has
displaced TFT_eSPI as the community default. TFT_eSPI's maintainer has effectively gone quiet
— a pinned GitHub issue on Bodmer/TFT_eSPI (
[#3770](https://github.com/Bodmer/TFT_eSPI/issues/3770)) states outright "this library is
abandoned - suggest LovyanGFX as the next suitable replacement," and community consensus
(LVGL forum, Arduino forum threads) is that LovyanGFX is ~90% source-compatible with
TFT_eSPI code, renders faster, tracks current Arduino-ESP32 core releases better, and — key
for CYD specifically — lets you set all pin/driver config **in your sketch** rather than by
hand-editing a library-wide `User_Setup.h`, which is the #1 source of "works for me, not for
you" CYD tutorial confusion. Arduino_GFX (moononournation) is a third option, popular for
boards LovyanGFX doesn't cover, but has smaller CYD-specific mindshare than LovyanGFX.

**rzeldent/platformio-espressif32-sunton**: a set of PlatformIO board-definition JSON files
(one per Sunton/CYD model, e.g. `esp32-2432S028R.json`, `esp32-2432S028Rv3.json` for the
2-USB/ST7789 board, `esp32-2432s024.json`, `esp32-2432s035.json`, etc.) meant to be dropped
into a project's `boards/` directory as a git submodule so PlatformIO auto-detects the right
board. Inspecting `esp32-2432S028Rv3.json` directly shows it bakes in: ST7789 driver on
SPI2_HOST @ 24 MHz, 240×320, RGB565, a 65536-byte DMA buffer, XPT2046 touch on SPI3_HOST @
2 MHz with X-axis mirroring and IRQ on GPIO 36, backlight on GPIO 21, and matching LVGL buffer
config (display width×height÷4, internal 8-bit-capable RAM, DMA queue depth 12). This is
consumed by the companion project
[rzeldent/esp32-smartdisplay](https://github.com/rzeldent/esp32-smartdisplay), which gives one
shared LVGL abstraction across every Sunton board variant — useful if you want to write once
and not worry about which of the CYD sub-variants ends up in a user's hands.
- **Pros for this project:** biggest community, most examples for exactly this hardware
  (WiFiManager, LVGL+HTTPS client examples, ArduinoJson, ESP32 SD library all mature and
  well-documented in the Arduino ecosystem), OTA via `ArduinoOTA`/`Update.h` is a few lines,
  captive-portal libraries (WiFiManager) are turnkey.
- **Cons:** Arduino-ESP32's HTTPS client (`WiFiClientSecure` + `HTTPClient`) is a thinner,
  less configurable wrapper around mbedTLS than ESP-IDF's `esp_http_client`/`esp-tls` — memory
  tuning knobs (buffer sizes, dynamic buffers) are less exposed. Larger baseline flash/RAM
  overhead than raw ESP-IDF because of Arduino-core scaffolding.
- **Host toolchain (Ubuntu):** `pip install platformio` (or the VS Code PlatformIO extension);
  first build downloads the `espressif32` platform package (~1–1.5 GB with toolchain +
  framework-arduinoespressif32 + tool-esptoolpy) into `~/.platformio`. Build time: a clean
  LVGL+Arduino build is typically 60–120 s on a modern laptop; incremental builds a few
  seconds.

### (b) Pure ESP-IDF (C) + LVGL via `esp_lvgl_port` / `esp_lcd`

`esp_lvgl_port` (Espressif's own component, in the ESP Component Registry, confirmed
compatible with LVGL 9 — "all types and functions used from LVGL9") wraps `esp_lcd` panel
drivers (built-in `esp_lcd_ili9341`, third-party `esp_lcd_st7789`/`esp_lcd_st7796` components)
and hands you a ready LVGL display + input device with sane double-buffering and an internal
LVGL task, so you're not hand-rolling the flush callback. Reference project structure:
[sukesh-ak/BSP-IDF5-ESP_LCD-LVGL9](https://github.com/sukesh-ak/BSP-IDF5-ESP_LCD-LVGL9).
- **Memory advantages:** no Arduino-core overhead; you control `sdkconfig` directly, including
  `CONFIG_MBEDTLS_DYNAMIC_BUFFER`, TLS buffer sizes, lwIP TCP/UDP PCB counts, and can trim
  unused ESP-IDF components entirely (Bluetooth stack, etc.) at build time. Practically this
  buys back tens of KB versus Arduino's fixed-size defaults.
- **TLS:** `esp-tls` (wrapping mbedTLS) + `esp_http_client`, with first-class support for
  `esp_crt_bundle` (a curated CA bundle) so you don't have to embed a specific cert for a
  transit API's HTTPS endpoint.
- **HTTP server:** `esp_http_server` — the same component ESP-IDF examples use for config web
  UIs; supports websockets if you ever want live-updating stats without polling.
- **SD:** `esp_vfs_fat` + `sdmmc`/`sdspi` host driver — standard FATFS-over-SPI, well trodden.
- **OTA:** `esp_https_ota` (single call for a full HTTPS OTA flow) or manual `esp_ota_ops` for
  more control.
- **Pros for this project:** most memory headroom and most control over the exact HTTPS
  connection lifecycle (open pooled connection every 30 s, close it, vs. holding it open) —
  which matters a lot given the tight RAM budget (§4). This is the stack where "run LVGL + a
  30-second HTTPS poll + a config web server + SD logging all at once, on a no-PSRAM ESP32" is
  most plausible without constantly fighting fragmentation.
- **Cons:** steeper learning curve, more boilerplate (menuconfig, component registration,
  CMakeLists per component), CYD-specific example code is scarcer than Arduino — you'll be
  adapting a generic `esp_lcd`+ILI9341/ST7789 example plus the CYD pin map from §2 yourself
  rather than copy-pasting a CYD-specific ESP-IDF tutorial (most CYD tutorials in the wild are
  Arduino).
- **Host toolchain (Ubuntu):** install via `install.sh`/`export.sh` from the `esp-idf` repo
  (targets `esp32`), or the newer `idf-tools.py`. Disk footprint: ESP-IDF checkout + toolchain
  + Python venv is roughly **3–5 GB** (bigger than PlatformIO's footprint because it includes
  the full toolchain for all supported chip targets unless you restrict `IDF_TARGET`). Build
  time: clean build of an LVGL+Wi-Fi+HTTPS+SD project is commonly 2–4 minutes on a modern
  laptop with `ccache` enabled (on by default in recent ESP-IDF); incremental is a few seconds.

### (c) Rust via esp-rs

- **Working CYD example in Rust?** The witnessmenow repo's own CI history shows a "functional
  touchscreen example in rust" commit (
  [Actions run](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/actions/runs/10620271259)),
  so *a* Rust touchscreen example exists in the canonical repo, but it's not the primary,
  actively-maintained path there — most examples and community support are Arduino-first. For
  the pieces you'd need: `mipidsi` (ILI9341/ST7789/ST7796 driver over `embedded-hal` SPI, with
  `embedded-graphics` as the drawing target) is the standard Rust MIPI-DBI display crate and is
  commonly paired with `embedded-graphics`; `xpt2046` crates exist on crates.io for the touch
  controller. I did not find a single actively-maintained, end-to-end "CYD in Rust with LVGL +
  HTTPS + web server + SD" reference project — you would be integrating several separate,
  independently-maintained crates yourself (display + touch + Wi-Fi + HTTP server + SD),
  which is a materially different effort level than (a) or (b) where a single community example
  usually already does most of that integration.
- **LVGL from Rust:** possible via the `lvgl` crate (bindgen bindings to the C library) but
  this is a thinner, less-travelled path than using `embedded-graphics` directly for a fairly
  simple "list of rows + header" UI — for this project's actual UI complexity (a handful of
  arrival rows), hand-drawn `embedded-graphics` primitives/text may be simpler than wiring up
  LVGL's C FFI from Rust.
- **How mature is Xtensa Rust in 2026?** The classic ESP32 is Xtensa (not RISC-V), so this
  project is squarely in the part of esp-rs that is **still the fork, not upstream**. Concretely:
  - `espup` remains the standard installer for the Xtensa toolchain (a patched LLVM +
    patched rustc, per [esp-rs/rust-build](https://github.com/esp-rs/rust-build)), installed
    *alongside* your normal rustup toolchain and selected per-project.
  - Espressif has an open effort to upstream the Xtensa backend into LLVM proper
    ([espressif/llvm-xtensa#13](https://github.com/espressif/llvm-xtensa/issues/13),
    [LLVM RFC thread](https://discourse.llvm.org/t/rfc-request-for-upstream-tensilica-xtensa-esp32-backend/65355)),
    with real progress ("most of the base ISA now in LLVM proper" per esp-rs's own blog,
    [mabez.dev Dec 2024 update](https://mabez.dev/blog/posts/esp-rust-12-04-2024/)) — but I
    found **no confirmation that this is complete or that upstream stable rustc has dropped
    the need for the esp-rs fork for Xtensa targets**. One search result summary claimed
    "xtensa esp-idf targets have been upstreamed to stable Rust since 1.81" —
    **I could not verify this claim against a primary source (esp-rs blog, rust-lang release
    notes) and it conflicts with the espup/rust-build documentation still describing a forked
    toolchain as of the sources I could access; treat it as unverified/likely incorrect.**
    Practically: **budget for installing `espup` and a separate Xtensa Rust toolchain**, not
    plain `rustup target add`.
  - RISC-V ESP32 chips (S3/C3/etc.) *do* use upstream rustc + `esp-idf-svc` std today without
    a forked compiler — but that's not this hardware.
- **Effort delta, honestly:** for this specific project (CYD, classic ESP32/Xtensa,
  HTTPS+LVGL-ish UI+SD+web server+captive portal), Rust is the **highest-effort option of the
  four**. You gain memory-safety and Cargo's dependency management, but you pay for it in (1)
  Xtensa toolchain friction (espup, a forked compiler, occasional version-pairing issues
  between esp-hal/esp-idf-svc and the toolchain), (2) thin CYD-specific prior art versus
  Arduino's abundance, and (3) stitching together independent crates for display+touch+SD+HTTP
  server rather than using one community-maintained integration. `esp-idf-svc` (the `std`
  path, wrapping ESP-IDF) is the more turnkey Rust option than `esp-hal` (the `no_std` path)
  for this feature set, since it gets you ESP-IDF's Wi-Fi, `esp_http_server`, and VFS/SD
  support "for free" via bindings rather than reimplementing them in `no_std`.
- **Host toolchain (Ubuntu):** `rustup` (normal Rust) + `espup install` (downloads the Xtensa
  LLVM/rustc fork, several hundred MB) + `cargo install espflash`/`cargo install ldproxy` (for
  the `esp-idf-svc` std path, which also requires an ESP-IDF checkout under the hood, similar
  disk cost to option (b), roughly **3–5 GB** total including the Xtensa toolchain and ESP-IDF).
  Build time: Rust + ESP-IDF sys-bindings clean builds tend to run **longer** than raw C
  ESP-IDF builds (linking `esp-idf-sys`'s bindgen output is not fast) — commonly 3–6 minutes
  clean, single-digit seconds incremental.

### (d) ESPHome (YAML + lambdas)

- **What ESPHome already gives you for free:** Wi-Fi provisioning with a **built-in captive
  portal** (`captive_portal:` component, zero extra code), OTA out of the box, an `sdcard`
  external component with a small **built-in web file-browser page** for upload/download/
  delete ([n-serrette/esphome_sd_card](https://github.com/n-serrette/esphome_sd_card),
  [andrewbackway/esphome-sd_card_logger_web](https://github.com/andrewbackway/esphome-sd_card_logger_web)
  — the latter specifically does high-frequency, non-blocking SD logging via a
  producer/consumer FreeRTOS queue), `http_request:` component for polling a JSON endpoint,
  and LVGL support has landed in ESPHome as a first-class component (you configure LVGL
  widgets largely in YAML, with `lambda:` blocks for anything dynamic like updating arrival
  text). The **ESPHome Device Builder** (its new web-based config/flash UI) hit 1.0.0 in the
  2026.6.0 release, replacing the legacy dashboard — flashing/config for end users is
  genuinely turnkey. **[ESPHome 2026.x changelogs — checked at a summary level, not read in
  full]**
- **Where it falls short for this project:**
  - **Custom configuration web UI** beyond what `captive_portal`/the native API give you: doing
    something bespoke (e.g. a page for the user to type in specific stop IDs / route filters)
    means either living inside ESPHome's fairly rigid web-server component (limited templating)
    or writing a full external C++ component that reimplements `esp_http_server` routes by
    hand — at which point you're not really saving effort over ESP-IDF/Arduino, you're doing
    the same work inside YAML's constraints.
  - **SD logging** is achievable (per the components above) but as *external components* — i.e.
    community add-ons, not core, so you inherit their maintenance risk and have to vet them
    yourself rather than relying on ESPHome core guarantees.
  - **On-screen statistics** (anything beyond simple sensor/label widgets — e.g. a scrolling
    history graph of past arrivals, or per-route computed stats) pushes past what LVGL-in-YAML
    comfortably expresses; you'd be writing increasingly large `lambda:` C++ blocks, at which
    point ESPHome's value-add (declarative YAML) erodes and you're effectively hand-writing
    C++ inside a YAML string.
  - Fine-grained control over the HTTPS client's memory behavior (buffer sizes, connection
    reuse) is not exposed at the YAML level — you inherit whatever ESPHome's `http_request`
    component defaults to.
- **Verdict:** ESPHome is plausible for a *simpler version* of this project (single JSON poll →
  a few LVGL labels, standard Wi-Fi setup, no bespoke config UI, minimal SD logging) but not a
  comfortable fit once "on-device web server for configuration" and "on-screen statistics"
  are both hard requirements, per the brief.
- **Host toolchain (Ubuntu):** `pip install esphome` (a few hundred MB with its Python deps);
  end users don't need a toolchain at all — they flash via the ESPHome Device Builder web UI
  (browser WebSerial/ESP Web Tools-based) or `esphome run`. Build time: ESPHome compiles to
  ESP-IDF or Arduino under the hood, so build times resemble (a)/(b) once the underlying
  platform is cached (~1–3 minutes).

### Flashing prebuilt binaries to end users (applies to a/b/c; d has its own flow)

**ESP Web Tools** (`esphome/esp-web-tools`, used via a small `<esp-web-install-button>` custom
element on a static page, e.g. hosted on GitHub Pages) is the standard way to let a non-technical
user flash a prebuilt binary from Chrome/Edge over WebSerial, no local toolchain required.
Manifest format (`manifest.json`):
```json
{
  "name": "Transit Display Firmware",
  "builds": [
    {
      "chipFamily": "ESP32",
      "improv": true,
      "parts": [
        { "path": "bootloader.bin",  "offset": 4096 },
        { "path": "partitions.bin",  "offset": 32768 },
        { "path": "firmware.bin",    "offset": 65536 }
      ]
    }
  ]
}
```
Offsets are given **in decimal** in the manifest even though ESP32's canonical map is usually
written in hex: bootloader **0x1000** (4096), partition table **0x8000** (32768), app image
**0x10000** (65536) — matching this project's brief exactly. Paths are resolved relative to the
manifest's own URL (or can be absolute URLs to another host). `improv: true` additionally lets
the page do Wi-Fi provisioning over the same serial connection immediately after flashing
(Improv Wi-Fi protocol), which is a nice complement to whatever captive-portal fallback you
build for users who don't use the web flasher.
Sources: [esphome/esp-web-tools](https://github.com/esphome/esp-web-tools),
[witnessmenow/ESP-Web-Tools-Tutorial](https://github.com/witnessmenow/ESP-Web-Tools-Tutorial).

---

## 4. Memory budget reality check (classic ESP32, no PSRAM)

The classic ESP32 has ~520 KB total SRAM, but a meaningful chunk is claimed by the ROM/BSS/
IRAM/DRAM split before your application heap ever starts; in practice **usable heap once
Wi-Fi + TLS + your app are all running commonly lands in the 100–160 KB range** rather than
the full 320 KB DRAM figure often quoted for the chip in isolation. Concrete data points found:

- **LVGL 9 buffer for a 320×240 panel, RGB565, 2×10-line partial buffers** (a typical
  LVGL "full CPU render, small buffer" config): 320 px × 10 lines × 2 bytes/px = **6,400 bytes
  per buffer**, ×2 buffers = **12,800 bytes (~12.5 KB)** total for the draw buffers themselves.
  This is small and not the dominant cost — LVGL's own object/style/font overhead and its
  internal allocator arena matter more once you have more than a trivial number of widgets.
  Rzeldent's board defs default to a **larger** buffer (`width×height÷4` = 320×240÷4 = 19,200
  px = **38,400 bytes** at RGB565) plus a 65,536-byte DMA buffer for the ILI9341/ST7789 driver
  — i.e. real-world configs often trade more RAM for smoother/faster rendering than the
  minimal 2×10-line approach.
- **LVGL 8→9 migration overhead:** community reports on the LVGL forum put LVGL 9.1's baseline
  memory footprint at roughly **5–6 KB more** than LVGL 8.3 for a comparable app — a real but
  fairly small tax versus staying on v8.
  ([LVGL forum thread](https://forum.lvgl.io/t/lvgl-9-higher-memory-usage-and-different-usage-reports-in-lvgl-9-compared-to-8/15308))
- **mbedTLS per HTTPS connection — this is the big one:**
  - `mbedtls_ssl_setup()` allocates **two independent buffers of ~16,717 bytes each** (one
    inbound record buffer, one outbound) — i.e. roughly **33 KB just for the record buffers**
    of a single TLS session, before accounting for the session/context struct itself.
  - Rule-of-thumb total commonly cited: **~33–35 KB heap + ~5–6 KB stack per active TLS
    session**.
  - The **handshake itself** transiently needs **~40–50 KB of free heap** (temporary
    allocations during the handshake that are freed afterward) — this transient spike, not the
    steady-state connection cost, is usually what actually causes OOM crashes on a loaded
    device.
  - Espressif's own guidance for `esp_https_ota` notes the **default mbedTLS Rx buffer is
    16 KB**, reducible to **4 KB** via `partial_http_download`, saving **~12 KB** — the same
    kind of buffer-size tuning (`CONFIG_MBEDTLS_DYNAMIC_BUFFER`, explicit smaller
    `MBEDTLS_SSL_IN/OUT_CONTENT_LEN`) is directly applicable to a small JSON-polling HTTPS
    client, and is one of the most impactful knobs available on ESP-IDF (much less exposed on
    Arduino's `WiFiClientSecure`).
  - After roughly "**~120 requests**" without properly freeing per-request allocations, one
    forum poster hit OOM — a caution about connection lifecycle management (close/free the TLS
    session between polls, or reuse one session carefully) rather than about steady-state cost
    per se.
- **Can you run LVGL + Wi-Fi + one TLS connection + a web server + SD FATFS concurrently on
  classic ESP32 with no PSRAM?** The honest answer from the sources found: **it's tight but
  commonly done, with real fragmentation/ordering hazards**, not a solved, worry-free
  combination:
  - Wi-Fi's own driver buffers are a fixed, non-trivial tax; one documented mitigation is
    disabling `CONFIG_ESP32_WIFI_IRAM_OPT` / `CONFIG_ESP32_WIFI_RX_IRAM_OPT` to reclaim ~27 KB
    (trading a little Wi-Fi throughput/latency for DRAM headroom) — directly relevant here
    since this project's Wi-Fi throughput needs are minimal (one small JSON GET every 30 s).
  - PSRAM would be the easy fix but this hardware doesn't have it, and even where PSRAM exists
    on *other* ESP32 boards, community reports flag that PSRAM isn't DMA-capable on classic
    ESP32, so SD-card DMA transfers and Wi-Fi buffers still need internal DRAM regardless —
    i.e. PSRAM wouldn't have fully solved this even if present.
  - Practical recipe that shows up repeatedly across the sources: keep the **HTTPS connection
    short-lived** (open, GET, parse, close — never held open across the 30 s idle gap), keep
    LVGL's buffers small (partial buffer, not a full 320×240 framebuffer, which would itself be
    150 KB at RGB565 — clearly untenable on this chip), keep the web server's own buffers
    minimal (few concurrent handlers, small `httpd_config_t` stack/buffer sizes), and open the
    SD card + serve the web UI + poll HTTPS as **sequenced, not fully concurrent, high-water-mark
    events** rather than assuming all four subsystems peak simultaneously. This favors the
    ESP-IDF path (b) over Arduino (a) for this project specifically, because ESP-IDF exposes
    the buffer-size knobs (mbedTLS Rx/Tx length, `esp_http_server` config) needed to make this
    fit reliably, and exposes heap-tracing (`heap_caps_get_free_size`,
    `heap_trace_start`) to verify it empirically on real hardware rather than guessing.
  - No single source gave a definitive "yes, verified, X KB free heap steady-state" number for
    *this exact combination* (LVGL+HTTPS+webserver+SD, no PSRAM) — treat the above as a
    composite of individually-verified per-subsystem costs, not a directly-cited end-to-end
    benchmark. **Recommend validating with `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`
    logged at runtime once a prototype exists**, rather than trusting a budget on paper.

---

## 5. Flashing from Linux

- **esptool.py** (bundled with both PlatformIO and ESP-IDF, or `pip install esptool`
  standalone) auto-detects the ESP32 over `/dev/ttyUSB0`-style CH340 ports.
- **CH340 auto-reset:** esptool resets the ESP32 automatically by toggling the CH340's DTR and
  RTS lines, which are wired to the ESP32's GPIO0 and EN pins — voltage changes on those two
  lines put the chip into firmware-download mode without any button press. **On genuine CH340
  adapters this generally works out of the box**; the "must hold BOOT" failure mode shows up on
  serial adapters/boards that only toggle one of DTR/RTS (not both), which doesn't correctly
  pulse EN — if auto-reset fails on this particular CYD unit, hold BOOT (GPIO 0, per §2) while
  esptool initiates the connection, or add `--before default_reset --after hard_reset` flags
  explicitly / fall back to `--before no_reset` and manual BOOT-hold if the automatic sequence
  is unreliable on this specific board revision.
- **Baud rate:** **921600 is the fastest commonly-recommended rate and does work on most CH340
  + ESP32 combinations**; if you see flashing errors/corruption, step down through
  460800 → 115200 (460800 is often cited as the more universally "rock solid" default,
  with 921600 as the "try it, fall back if flaky" first choice, particularly on cheaper CH340
  clones).
- **Linux dialout group:** the CH340 enumerates as `/dev/ttyUSB0` (or similar) owned by
  `root:dialout` by default; the user account needs to be in the `dialout` group
  (`sudo usermod -aG dialout $USER`, then re-login) to access it without `sudo` — the common
  one-off workaround is `sudo chmod a+rw /dev/ttyUSB0`, but that doesn't persist across
  replugs/reboots, so the group membership is the correct fix for a dev workflow.
- **Typical flash command shape** (matches the partition offsets from §3):
  ```
  esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash \
    0x1000  bootloader.bin \
    0x8000  partitions.bin \
    0x10000 firmware.bin
  ```

Sources: [esptool boot-mode-selection docs](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html).

---

## 6. Fonts and UI

- **LVGL built-in Montserrat sizes:** LVGL ships pre-rendered bitmap fonts named
  `lv_font_montserrat_<size>` at a standard set of sizes (8/10/12/14/16/18/20/22/24/26/28/30/
  32/34/36/38/40/42/44/46/48, per LVGL's built-in-fonts doc), each built at bpp=4
  (4 bits/pixel anti-aliasing) by default. You enable the ones you need via
  `LV_FONT_MONTSERRAT_<size>` config defines to avoid pulling all of them into flash.
- **Custom fonts:** convert any TTF/WOFF/OTF via **`lv_font_conv`** (Node-based CLI, also has a
  web UI) into LVGL's compact bitmap C-array format. It supports bpp 1/2/4/8 (higher bpp = much
  larger binary — bpp=4 is roughly 4× the size of bpp=1), preserves kerning, has built-in
  compression, and — importantly for a transit display — lets you **subset to only the
  glyphs you need** (digits, route-letter/number badges, a limited destination-name character
  set) to keep flash usage down. Typical invocation:
  ```
  lv_font_conv --font MyFont.ttf --size 20 --bpp 4 --format lvgl \
    --output my_font_20.c -r 0x20-0x7F
  ```
  (`-r 0x20-0x7F` restricts to basic ASCII — extend the range if you need accented characters
  in destination names.) Espressif/LVGL also ship a `built_in_font_gen.py` helper if you want
  Montserrat itself at a non-standard size.
- **Does 320×240 comfortably fit ~6 arrival rows + header?** Yes, with normal margins. Rough
  budget at 240 px of vertical space (landscape 320×240, header across the top): a ~28–32 px
  header (route/clock/Wi-Fi-status icon) leaves ~208–212 px for rows; **6 rows at ~34 px each**
  (32 px row + ~2 px gap) fits in 204 px, comfortably under budget, using something like
  Montserrat 16–18 for the row text (route badge + destination truncated/ellipsized + right-
  aligned minutes) and Montserrat 12–14 for a secondary line if you want a two-line row (route
  + destination on line 1, "in N min" on line 2) instead. Landscape 320-px width is the tighter
  constraint for text (destination names will often need truncation/marquee-scroll rather than
  full display), not row count.
- **Backlight/power:** as noted in §2, the CYD's backlight pin (GPIO 21 on the 2.8"/24" boards,
  GPIO 27 on the 3.5" board) is a **plain digital GPIO**, not pre-wired to a PWM peripheral —
  you attach it to one of the ESP32's LEDC PWM channels yourself
  (`ledcSetup`/`ledcAttachPin` in Arduino, `ledc_channel_config`/`ledc_timer_config` in
  ESP-IDF) to get smooth dimming rather than a binary on/off. A 12-bit LEDC timer resolution is
  a common choice for smooth perceived dimming curves.
- **LDR auto-dimming:** the onboard LDR (GPIO 34, analog/ADC1) gives a crude ambient-light
  reading — `analogRead()`/`adc1_get_raw()` — suitable for a coarse day/night backlight curve
  (e.g. map raw LDR reading to a small number of brightness bands with hysteresis to avoid
  flicker at the boundary) rather than fine photometric accuracy; several community CYD
  projects implement exactly this pattern (LDR read → non-linear/perceptual brightness curve →
  LEDC duty cycle), per the witnessmenow repo's own `BacklightControlTest`/`LDRTest` examples
  and community projects like
  [nicholaswilde/cyd-weather-station](https://github.com/nicholaswilde/cyd-weather-station).

---

## 7. Wi-Fi provisioning + config UI patterns

- **WiFiManager (tzapu/WiFiManager, Arduino)**: the pragmatic default for a hobbyist/small-batch
  project. On boot, tries the last saved AP; on failure, puts the ESP32 into AP mode, spins up
  a DNS server (captive-portal DNS hijack — all DNS queries resolve to the device's own IP) and
  a small web server with a network-picker form. Zero external dependencies beyond the library,
  a UX pattern users already know from hotel/airport Wi-Fi. This is almost certainly the right
  choice if you go the Arduino route (§3a) — it directly supports adding **custom config
  fields** to the same captive-portal page (WiFiManager's `WiFiManagerParameter`), which is
  convenient for also capturing e.g. "which transit stop ID" during initial setup, one form,
  one flow.
- **ESP-IDF's native `wifi_provisioning` component**: adds SoftAP-based provisioning *and*
  **BLE-based provisioning** (paired with Espressif's mobile provisioning apps), more at home
  if you're already fully in ESP-IDF (§3b) rather than mixing in an Arduino-style library. BLE
  provisioning is generally the better UX for a polished consumer product (companion app,
  no captive-portal browser quirks on iOS/Android) but is more setup than this project likely
  warrants for a personal desk-display; SoftAP + captive portal fits the "maker project" profile
  described in the brief more naturally, per general community guidance found.
- **Custom captive portal (manual DNS hijack + `esp_http_server`/`ESPAsyncWebServer`)**: gives
  full control over the config page (styling, additional fields, showing live stats during
  setup) at the cost of writing the DNS-hijack server yourself — not hard (a "respond to every
  query with your own IP" DNS server is ~50 lines), reference implementations exist for both
  Arduino (`espressif/arduino-esp32`'s own `DNSServer` `CaptivePortal` example) and ESP-IDF
  ([achimpieters/esp32-captive_portal](https://components.espressif.com/components/achimpieters/esp32-captive_portal),
  which bundles DNS hijack + HTTP server + mDNS together). Given this project explicitly wants
  a **persistent, non-setup-only config web UI** (route/stop configuration, viewing stats), the
  cleanest architecture is likely: **captive portal only for first-boot Wi-Fi setup**, then a
  **separate, always-on config page** served by the same `esp_http_server`/`ESPAsyncWebServer`
  instance once on the home network (reachable via mDNS, below) — i.e. don't try to make the
  captive-portal page double as the permanent config UI; they have different lifecycles and
  audiences.
- **Where to store config — NVS vs LittleFS JSON vs SD:**
  - **NVS** (ESP-IDF's built-in key-value flash store, also usable from Arduino via
    `Preferences.h`) is the right place for small, frequently-read scalars: Wi-Fi
    credentials, stop ID, poll interval, brightness curve settings. Wear-levelled, small, no
    filesystem overhead, survives OTA (if you don't erase the NVS partition).
  - **LittleFS JSON** is better for structured/larger config (e.g. a list of multiple stops/
    routes with display names) that's easier to read/write as a JSON blob than to flatten into
    NVS keys, and is convenient because you can serve/accept the same JSON file directly from
    the web config UI (`GET /config.json`, `POST /config.json`) with minimal
    marshalling.
  - **SD** is the wrong place for live config (it's meant for this project's *logging*
    workload, and mixing config reads/writes with a removable card users might pull out is
    fragile) — reserve SD purely for the arrival-history log the brief calls for, and keep
    config in flash (NVS/LittleFS) so the device works even with no card inserted.
- **mDNS**: both Arduino (`ESPmDNS.h`, `MDNS.begin("transit-display")`) and ESP-IDF
  (`mdns_init()` + `mdns_hostname_set()`) support advertising a `.local` name so the user can
  reach the config UI at `http://transit-display.local` instead of hunting for a DHCP-assigned
  IP — works out of the box on macOS/iOS/Linux (all ship a mDNS/Bonjour resolver) and on modern
  Windows 10/11 (mDNS support built into recent Windows), though some older/locked-down
  Android devices historically have flaky mDNS resolution in the browser — worth surfacing the
  device's IP address as a fallback (e.g. printed on the CYD's own screen) rather than relying
  on `.local` alone for every user.

---

## 8. Recommendation

**Primary recommendation: ESP-IDF (C) + `esp_lvgl_port`/`esp_lcd` for the display, `esp_http_client`/`esp-tls` for the 30 s HTTPS poll, `esp_http_server` for the config UI, `esp_vfs_fat`+SPI SD for logging, and WiFiManager-style SoftAP captive portal for first-boot Wi-Fi (hand-rolled DNS hijack + `esp_http_server`, or the `achimpieters/esp32-captive_portal` component).**

Reasoning: this project stacks four RAM-hungry subsystems (LVGL, a TLS client, a web server,
FATFS/SD) on a chip with no PSRAM and a genuinely tight usable-heap budget once Wi-Fi and TLS
are both live (§4). Of the four stack options, ESP-IDF is the only one that exposes the
specific knobs known to matter here — mbedTLS Rx/Tx buffer sizing, dynamic TLS buffers, IRAM
Wi-Fi trade-offs, heap tracing to *verify* the budget on real hardware rather than hope — while
still having first-class, actively-maintained components for every piece of this project
(display, HTTP client+server, SD, OTA) and a large enough non-CYD-specific ESP-IDF community
that generic "esp_lcd + ILI9341/ST7789" and "esp_http_server config page" examples are easy to
find and adapt. It costs more boilerplate up front than Arduino, but that cost is paid once,
while the memory headroom it buys back is paid back continuously for the life of the project
every time the device does its 30 s poll next to a live web-server connection.

**Fallback: PlatformIO + Arduino + LVGL 9 + LovyanGFX**, specifically if the ESP-IDF learning
curve proves too costly against the timeline, or if the memory budget turns out to have more
headroom in practice than §4's numbers suggest (worth prototyping the ESP-IDF path first
*specifically to find out*, since it's cheap to measure with `heap_caps_get_free_size` before
committing). The Arduino/LovyanGFX path has by far the largest body of CYD-specific example
code, WiFiManager is turnkey and already speaks the exact "captcaptive-portal-plus-custom-
fields" pattern this project's Wi-Fi-and-stop-ID setup needs, and ArduinoOTA/community LVGL+
HTTPS examples mean faster initial progress — the tradeoff is that if/when the four subsystems
do start fighting over heap, Arduino's thinner abstractions over Wi-Fi/mbedTLS give you fewer
levers to pull without dropping into ESP-IDF-level APIs anyway (Arduino-ESP32 is itself built
on ESP-IDF, so this is always an escape hatch, not a dead end).

**Explicitly not recommended for this project (with reasons already detailed above):**
Rust/esp-rs (§3c) — the classic ESP32 is Xtensa, meaning a forked toolchain via `espup` rather
than mainline `rustc`, plus the least CYD-specific and least integrated prior art of the four
options; a defensible choice if the user's priority were "learn/use Rust" specifically, but the
brief doesn't signal that, and the effort delta is real. ESPHome (§3d) — excellent for a
simpler version of this project, but the brief's bespoke config web UI, SD logging, and
on-screen statistics push past what YAML+lambdas comfortably express before you're
"fighting the framework" as much as building on it.

---

## Open items / unverified flags recap
- **[UNVERIFIED]** USB-C-lacking-CC-resistors claim for the 2-USB CYD variant (atomic14.com,
  not cross-checked against a schematic or second independent source).
- **[UNVERIFIED]** ESP32-2432S024's touch-CS/backlight-pin divergence from the 2432S028R map
  (espboards.dev/esp32s.com summaries, no primary schematic found).
- **[UNVERIFIED]** ESP32-3248S035 full pin map (espboards.dev/esp32pins.com aggregator
  agreement, but no Sunton-official schematic located) — confirm against the physical unit
  with a multimeter/continuity check or software ID probe before committing wiring in code if
  a 3.5" unit is ever used instead of the on-hand device.
- **[UNVERIFIED / likely incorrect]** the claim that "Xtensa esp-idf targets have been
  upstreamed to stable Rust since 1.81" — conflicts with espup/esp-rs/rust-build still
  documenting a forked toolchain; treat Xtensa Rust as still requiring `espup` as of this
  research date unless you find a primary esp-rs/rust-lang announcement saying otherwise.
- No single primary source gave a directly-measured, end-to-end free-heap number for
  "LVGL + Wi-Fi + one TLS connection + web server + SD FATFS, all concurrent, no PSRAM" — §4's
  conclusion is a composite of individually-sourced per-subsystem costs, not one cited
  benchmark; validate empirically on the actual hardware early in the project.
- The on-hand device's exact CYD variant is **still unconfirmed** — it's only known to expose a
  CH340 (`1a86:7523`) USB-serial adapter, which is common to most CYD variants and does not by
  itself indicate display size, controller, or touch technology. Probe via §1's software
  identification steps (display ID register read, I2C scan for GT911/CST820, USB port count,
  silkscreen) once you have the unit in hand.
