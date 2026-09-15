# Philly Transit Display — firmware

PlatformIO project for the ESP32 "Cheap Yellow Display" family. See `../DESIGN.md` for the
full design (sections 3 and 5 cover hardware and firmware architecture); this file only covers
firmware-specific build notes not already in `platformio.ini`'s own comments or
`docs/hardware.md`.

## Building

```sh
export PATH="$HOME/.platformio/penv/bin:$PATH"
cd firmware
pio run -e cyd-3248S035R        # the owner's board; see platformio.ini for the other envs
pio test -e native              # transit_core + transit_stats host tests (64 cases)
```

A brand-new build directory may need `pio run` twice - see `platformio.ini`'s comment on
`extra_scripts` for why (the `esp32_smartdisplay` patch script runs before PlatformIO
downloads `lib_deps`, so the very first build of a fresh environment has nothing to patch yet).

`-DDEMO_DATA` (commented out in `platformio.ini`) makes `GET /api/state` and the LVGL main
screen render `demo_data.cpp`'s hardcoded `Snapshot` instead of the real one from
`net_poller.cpp` - useful for screen/UI work with no Wi-Fi or SEPTA reachable. It costs nothing
when unused: `demo_data.cpp` is only ever called from behind this flag, so the linker's
`--gc-sections` drops it entirely from a normal build (confirmed: removing the last
unconditional call site to `buildDemoSnapshot()` measurably shrank `firmware.bin`).

## Memory and flash budget

Measured on the owner's ESP32-3248S035R (classic ESP32, 4 MB flash, no PSRAM) on 2026-09-14.
The app partition (`firmware/partitions.csv`) is 1,900,544 bytes (`0x1D0000`) per OTA slot.

| Build | Flash | Static RAM |
|---|---:|---:|
| After the 2026-09-15 review fixes (`rf/data`: transport completeness, transit-first polling, per-stop health on the panels, checked SD writes, log export) | 1,769,374 B (93.1 %) | 95,332 B (29.1 %) |
| Same branch point, before those fixes | 1,760,398 B (92.6 %) | 95,268 B (29.1 %) |
| Everything incl. weather, Indego, profiles, night page, 48 px font | 1,858,446 B (97.8 %) | 95,300 B (29.1 %) |
| Same, before the second round of trims | 1,889,518 B (99.4 %) | |
| Weather only, before the first round | 1,897,974 B (99.9 %) | |

Flash headroom is about 175 KB on the 3.5" boards (2026-09-14, after dropping the HTTPS mode; it was 30 KB before that). `platformio.ini`'s comment and `include/lv_conf.h` list the
knobs (fonts, LVGL features, debug level); do not grow the app slots without dropping OTA.
Trims made 2026-09-14, first when the weather feature pushed the image to 99.9 % and again when
the interview features did:
- Montserrat 20 is only compiled for the 240-tall boards (their board files pass
  `-D LV_FONT_MONTSERRAT_20=1`; `lv_conf.h` defaults it to 0): 22 KB on the 3.5" build.
- `LV_DRAW_SW_SUPPORT_{RGB888,XRGB8888,L8,AL88,I1}` are 0: this UI never draws images, and each
  blend routine was 3-7 KB. 33 KB.
- `LV_DRAW_SW_COMPLEX` is 0: no shadows, transforms, or masks. LVGL then draws NOTHING for a
  rectangle with a radius (it only logs a warning), so every style keeps radius 0. ~12 KB.
- `CORE_DEBUG_LEVEL` 1: the project's own `[tag]` lines are plain `Serial.printf` and stay. ~4 KB.
- No `sscanf` anywhere (transit_core, weather_core parse by hand): drops newlib's float scanf.
- The big minutes font is chosen per board at compile time (2026-09-14, after the crowding icon
  font pushed the 2.4" capacitive build 2 KB over the slot): the 320x240 boards use Montserrat 20
  in both orientations and no longer link Montserrat 28 (~32 KB) just for portrait.
- `LV_LOG_USE_FILE_LINE` and `LV_LOG_USE_TIMESTAMP` are 0 (no `__FILE__` string at every LVGL
  warn/error site) and `LV_LABEL_TEXT_SELECTION` is 0. A few KB on every board.
  ~9 KB.
- The big-digits font is a 21-glyph subset (`src/fonts/README.md`), ~13 KB instead of ~60.
`LV_MEM_SIZE` is 36 KB (was 32; the night page, bike strip and crowding labels took the pool to
93 %, it is ~82 % used at boot now with ~6 KB spare for text changes). Static RAM the heap no
longer gets, so three other things were shrunk the same day to keep ~75-80 KB of heap free at
runtime with a 40+ KB largest block: the arrival tracker keeps 8 trips per stop instead of 12
(`transit_stats/tracker.h`, ~5 KB), the 3.5" draw buffer is 1/20 of the screen instead of 1/16
(board JSON, ~4 KB), and the UI caches the visible-stop list per build instead of copying the
stop vector several times a second.

Concurrency limit: the async web server handles one request at a time but queues the responses,
and with this little heap the fourth of four *simultaneous* `/api/state` requests can go out as
an HTTP 200 with an empty body (AsyncTCP's send buffer allocation fails; the device stays up -
measured 2026-09-14 with three rounds of seven concurrent requests, no reboot). The web UI issues
one state request at a time, so this only matters for scripted clients: treat an empty 200 as a
retry.

Gotcha: PlatformIO does not recompile the LVGL library when only `include/lv_conf.h` changes,
so a config edit can look like it had no effect. Run `pio run -t clean` (or delete
`.pio/build/<env>/lib*/lvgl`) before measuring. CI always builds clean.

### Heap, stage by stage (`[heap]` lines on the serial console at boot)

| After | Free | Largest block |
|---|---:|---:|
| display (LVGL + 19 KB draw buffer) | 216 KB | 110 KB |
| config + arrival tracker (~12 KB) + poller task stack (10 KB) | 178 KB | 110 KB |
| Wi-Fi connected | 127 KB | 86 KB |
| web server, mDNS, SNTP | 100 KB | 61 KB |
| SD card mounted | 69 KB | 32 KB |
| UI screens built | 68 KB | 31 KB |
| steady state while polling | ~86 KB | ~43 KB |

Rules that fell out of this, all learned the hard way (each one was a boot loop first):

- Anything large and long-lived (the tracker, task stacks) is allocated before Wi-Fi starts,
  while the heap is one contiguous block. Every `new` of a big object is `nothrow` and checked:
  with exceptions disabled a failed plain `new` calls `std::terminate()`.
- No TLS at all (v0.1.2 removed the opt-in HTTPS mode): a TLS session needs ~40 KB with two 16 KB
  contiguous buffers this board cannot spare, every service the firmware uses serves plain http,
  and dropping mbedTLS/x509/the CA bundle freed ~100 KB of flash.
- No second worker task: the poller drains the web job queue between polls. The AsyncTCP task
  stack is capped at 8 KB (`CONFIG_ASYNC_TCP_STACK_SIZE`; the library default is 16 KB).
- Proxied SEPTA bodies (stop lists up to ~18 KB) stream into a LittleFS temp file and are served
  from it; nothing network-sized is ever held in a growing buffer. There are two such files and
  each is *leased* to one response for its whole life (released by the request's disconnect
  callback); a third concurrent proxy job gets a 503 instead of overwriting a file someone is
  still reading.
- SD is mounted with `max_open_files = 2` and the poller holds one whenever it appends a row or
  scans a month for a stats summary, so log downloads take a single-reader lease
  (`acquireLogReader()` in `sd_logger.h`) and a second concurrent one is refused with a 503.
- Nothing on the LVGL task touches SD or the network. The stats page's `getStopSummary()` returns
  a cached value plus its age; the poller recomputes one stop per idle slice.
- LVGL's static pool is 32 KB (`LV_MEM_SIZE`); the draw buffer is 1/16 of the screen in RGB565
  (`LVGL_BUFFER_PIXELS` in `boards/*.json`). The `[lvmem]` boot line shows pool usage.
- The ESP32's static `.bss` budget is separate from, and much smaller than, the heap: a ~14 KB
  object declared at file scope fails to link ("DRAM segment data does not fit").

### Verifying a change

`pio run -e cyd-3248S035R -t upload`, then watch the serial console for `[heap]`, `[lvmem]`,
`[net_poller] free_heap=... largest_block=...` and `[proxy]` lines while hitting the API from the
LAN (stop-list proxies for several routes, `/api/stats`, a CSV download). A healthy device shows
no `rst:` lines and a largest block above ~30 KB between polls.
