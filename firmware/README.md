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
pio run -e cyd-3248S035R-https  # the same board with the HTTPS prototype compiled in (DESIGN.md §2.1)
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

## The web PIN

Everything that *changes* the device needs a per-device admin PIN in an `X-Pin` header
(`DESIGN.md` §7 lists exactly which routes; reading state, config and stats stays open). The PIN is
six digits, generated with the hardware RNG on first boot and kept in NVS (`ptd`/`pin`), not in
`/config.json` — `GET /api/config` is an open endpoint.

**Finding it** — two ways, both needing physical access to the display, which is the point:

- the serial console prints it once at boot: `[auth] web PIN: 123456`
  (`pio device monitor -e cyd-3248S035R`, then tap the reset button);
- the device info screen on the panel shows `Web PIN: 123456` (tap the screen twice from the
  arrivals page).

**Using it**

```sh
curl -H "X-Pin: 123456" -H "Expect:" -F firmware=@.pio/build/cyd-3248S035R/firmware.bin \
     http://transit-display.local/api/ota
curl -H "X-Pin: 123456" -X POST http://transit-display.local/api/reboot
curl -H "X-Pin: 123456" -H 'Content-Type: application/json' \
     -X PUT --data-binary @config.json http://transit-display.local/api/config
curl -H "X-Pin: 123456" -O http://transit-display.local/api/log/2026-09.csv
```

`-H "Expect:"` matters for the upload: curl sends `Expect: 100-continue` for a body this size, the
async server answers `100 Continue` and then drops the connection without reading the image, and
curl reports an empty reply (seen 2026-09-16; the device is untouched and stays on its old build).
With the header suppressed the same command answers `{"ok":true,...}` in about 25 s and reboots.

**Changing it** — 4 to 32 printable ASCII characters, no whitespace; the current PIN authenticates
the change:

```sh
curl -H "X-Pin: 123456" -H 'Content-Type: application/json' \
     -X POST -d '{"pin":"my-new-pin"}' http://transit-display.local/api/pin
```

**If it is lost**, there is no network reset — read it off the serial console or the panel as
above. It survives an OTA; only a full flash erase (`esptool.py erase_flash`, which also wipes the
stored Wi-Fi credentials and the config) generates a new one.

Five wrong PINs in a row lock every protected route for 30 seconds (HTTP 429, with `retry_s` in
the body). A *missing* header never counts towards that, so a script that has not been told about
the PIN cannot lock the owner out.

The setup AP has its own generated password (NVS `ptd`/`ap_pass`), shown on the panel with a QR
code when the portal is open. A device that already has Wi-Fi credentials does not open that
portal on its own — it retries its stored network with a 5 s → 60 s backoff and opens the portal
only when the screen is tapped (`DESIGN.md` §12).

## Memory and flash budget

Measured on the owner's ESP32-3248S035R (classic ESP32, 4 MB flash, no PSRAM); the top three rows
are 2026-09-15, the rest 2026-09-14.
The app partition (`firmware/partitions.csv`) is 1,900,544 bytes (`0x1D0000`) per OTA slot.

| Build (`cyd-3248S035R`) | Flash | Static RAM |
|---|---:|---:|
| 2026-09-16 screen pass (phone-style Wi-Fi bars, stats page with the on-time meter in three layouts, device page with SEPTA/SD health and the Data sources card, stats+device built on demand) on top of the exception pool and streamed `/api/state` | 1,840,258 B (96.8 %) | 95,652 B |
| Same tree without the screen pass (`next`, 2026-09-16) | 1,832,302 B (96.4 %) | 95,652 B |
| v0.2.0: the 2026-09-15 review fixes combined (PIN, Host check, OTA board check, WPA2 setup AP + QR, transport completeness, transit-first polling, per-stop health on the panels, checked SD writes, log export) | 1,828,384 B (96.2 %) | 95,644 B (29.2 %) |
| Hardening pass alone, with the QR widget compiled out | 1,767,926 B (93.0 %) | 95,580 B |
| Before the review fixes (v0.1.2) | 1,741,770 B (91.6 %) | 95,268 B |
| Everything incl. weather, Indego, profiles, night page, 48 px font | 1,858,446 B (97.8 %) | 95,300 B (29.1 %) |
| Same, before the second round of trims | 1,889,518 B (99.4 %) | |
| Weather only, before the first round | 1,897,974 B (99.9 %) | |

The tightest board tracks it closely: `cyd-2432S024C` is 1,835,858 B (96.6 %) / 95,788 B after the
screen pass (1,828,134 B before it, same day; the Data sources card never appears on its 240-tall
layouts but its code is linked); it was 1,781,266 B (93.7 %) / 95,716 B at v0.2.0 and
1,737,634 B (91.4 %) / 95,404 B before the hardening pass.

The screen pass is +8.0 KB on the 3.5" board (of which the Data sources card is 1.4 KB). Its text is composed with `snprintf` and LVGL inline
recolor commands (`ui_common.h colorHex`) rather than `std::string` concatenation, which was
measured at ~1 KB per page in string template instantiations; what remains is the layout code
itself (the `lv_obj_set_style_*` calls in the two `create*Screen()` functions).

The hardening pass cost 43,600 B on the 3.5" board, of which **~17.4 KB is the QR code** on the
Wi-Fi setup screen — `LV_USE_QRCODE` pulls in `qrcodegen.c` and `lv_qrcode.c`, which need
`LV_USE_CANVAS`, which needs `LV_DRAW_SW_SUPPORT_I1` for anything to actually be blended onto the
RGB565 draw buffer (without it the QR object exists and draws nothing, with no warning). Measured
by building with those three `lv_conf.h` flags off and the call sites stubbed. If flash gets tight
again, that is the single biggest item that can go: turning the three flags off and dropping the
`lv_qrcode_*` calls in `ui.cpp` leaves the setup screen showing the network name and password as
text, which still works — it is just ten characters to type.

Flash headroom is about 112 KB on the 3.5" boards and 116 KB on the 2.4" capacitive one.
`platformio.ini`'s comment and `include/lv_conf.h` list the
knobs (fonts, LVGL features, debug level); do not grow the app slots without dropping OTA.
Trims made 2026-09-14, first when the weather feature pushed the image to 99.9 % and again when
the interview features did:
- Montserrat 20 is only compiled for the 240-tall boards (their board files pass
  `-D LV_FONT_MONTSERRAT_20=1`; `lv_conf.h` defaults it to 0): 22 KB on the 3.5" build.
- `LV_DRAW_SW_SUPPORT_{RGB888,XRGB8888,L8,AL88}` are 0: this UI draws almost no images, and each
  blend routine was 3-7 KB. 33 KB. (`I1` was in that list until 2026-09-15; the QR code needs it.)
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
93 %, it is ~82 % used at boot now with ~6 KB spare for text changes). The Wi-Fi setup screen's QR
code takes ~2.5 KB of that pool for its 1-bit canvas, which fits only because that screen exists
before `ui::init()` has built the three real ones and the device reboots out of the portal either
way - do not move the QR anywhere that coexists with the arrivals page. Static RAM the heap no
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

**The "free" column overstates what a buffer can get, by about 34 KB** (found 2026-09-16 while
measuring HTTPS, DESIGN.md §2.1). `ESP.getFreeHeap()` - the number in the table, in `/api/state`'s
`heap`, and behind the 60 KB OTA gate, the 24 KB `/api/state` gate and the 40 KB idle-work gate - is
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`, and on the classic ESP32 that includes the IRAM heap
region that only 32-bit word access can use. `malloc()` never hands that region out for a string, a
body buffer or a TLS record. The byte-addressable heap (`MALLOC_CAP_8BIT`) on the owner's board is
**~46 KB free at idle and ~27-42 KB at the moment a fetch starts**, with the largest block already
measured in those terms. Since 2026-09-16 the `[heap]` boot lines and the `[net_poller]` heartbeat
print both (`free=` and `free8=`); the gates above still use the INTERNAL number and their thresholds
were tuned against it, so they are not wrong, but anyone sizing a new allocation should read `free8`.

Rules that fell out of this, all learned the hard way (each one was a boot loop first):

- Anything large and long-lived (the tracker, task stacks) is allocated before Wi-Fi starts,
  while the heap is one contiguous block. Every `new` of a big object is `nothrow` and checked:
  with exceptions disabled a failed plain `new` calls `std::terminate()`.
- No TLS in the shipping envs (v0.1.2 removed the opt-in HTTPS mode; dropping mbedTLS/x509/the CA
  bundle freed ~100 KB of flash): a TLS session on this SDK is two fixed 16,717 B contiguous record
  buffers plus ~20 KB around them, ~55 KB at the handshake peak, and the byte-addressable heap at
  the moment a fetch starts is 27-42 KB (above). The 2026-09-16 prototype (`cyd-*-https` envs,
  DESIGN.md §2.1) keeps a heap gate in front of every fetch and was measured on the owner's board:
  the gate never once found room, so the prototype behaves exactly like the shipping build while
  saying so in the UI. What HTTPS actually needs is a rebuilt SDK with smaller TLS buffers
  (§2.1 "what it would take"), not a bigger gate.
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
