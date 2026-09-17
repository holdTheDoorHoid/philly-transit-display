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
pio test -e native              # transit_core + transit_stats + poller-liveness host tests (184 cases)
pio run -e ui-sim               # host screenshot simulator (sim/README.md)
pio run -e ui-sim-pool          # the same, with LVGL's pool scaled to the board's (DESIGN.md §8)
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

**Both halves of that print to the console now** (2026-09-16): `[auth] wrong PIN on <method> <path>`
for each attempt, and `[auth] N wrong PINs in a row; every protected route is locked for 30 s` when
the lockout engages. They are plain `Serial.printf`, not `log_w`, because `CORE_DEBUG_LEVEL` is 1
and `log_w` compiles to nothing at that level - so until this change a device that locked every
setting-changing route for thirty seconds said nothing at all about it. The reason it was noticed:
a device-suite run reported eleven `PUT 429`s in a section that sends no wrong PIN anywhere, and
there was no evidence to work from. What the code says about that is narrow and worth writing down,
because it is most of the diagnosis: 429 has exactly one producer (five consecutive
`auth::Result::Wrong`), a missing header is `Missing` and never counts, and a correct PIN zeroes the
streak - so a client that always sends the right value can only get there if the `X-Pin` header
arrived **present but damaged**, five times running. It has not reproduced since (fourteen
hand-issued correct-PIN PUTs at the suite's cadence and body size, then two full suite runs, all
clean), so this is a hook for the next occurrence rather than a fix. If it does recur and the route
alone is not enough, log `strlen(provided)` beside it - an empty or short value would settle it
immediately, and a length leaks nothing.

The setup AP has its own generated password (NVS `ptd`/`ap_pass`), shown on the panel with a QR
code when the portal is open. A device that already has Wi-Fi credentials does not open that
portal on its own — it retries its stored network with a 5 s → 60 s backoff and opens the portal
only when the screen is tapped (`DESIGN.md` §12).

## Memory and flash budget

Measured on the owner's ESP32-3248S035R (classic ESP32, 4 MB flash, no PSRAM); the top six rows
are 2026-09-16/15, the rest 2026-09-14.
The app partition (`firmware/partitions.csv`) is 1,900,544 bytes (`0x1D0000`) per OTA slot.

| Build (`cyd-3248S035R`) | Flash | Static RAM |
|---|---:|---:|
| 2026-09-16 the two reboot paths (`src/app/cpu_yield.*` + the yield in `streamLogLines()`, the stats jobs answering through `json_response.h`, `HostGuardHandler` replacing the server middleware, `src/app/host_match.h`, the auth console lines), on top of `next` at baf20a0 | 1,861,742 B (98.0 %) | 96,460 B |
| The same `next` (baf20a0) without them - the baseline that delta is measured against | 1,862,942 B (98.0 %) | 96,404 B |
| 2026-09-16 the display task's zero-wait lock policy (`src/app/ui_lock.h`, the `LastGood` fallback in every shared accessor, the Snapshot published and read as `shared_ptr<const>`, `lock_misses`/`tick_ms` on `/api/debug/ui`), on top of `next` at 187b37e | 1,862,942 B (98.0 %) | 96,404 B |
| The same `next` (187b37e) without it - the baseline that delta is measured against | 1,859,434 B (97.8 %) | 95,932 B |
| 2026-09-16 release-candidate review fixes (the per-fetch liveness stamp, the transport-failure early exits, the OTA upload warm + guard + Host check, the row-scaled panel guard, `tryGetPollStatus()`, the `lvgl_pool` restart note), on top of `next` at ec0c1ab | 1,859,434 B (97.8 %) | 95,932 B |
| The same `next` (ec0c1ab) without them - the baseline that delta is measured against | 1,857,854 B (97.8 %) | 95,868 B |
| 2026-09-16 poller-liveness net (the cycle stamp, the display-loop check, `last_restart` + `last_poll.since_s` on `/api/state`), on top of `next` at 2f48828 | 1,856,922 B (97.7 %) | 95,868 B |
| The same `next` (2f48828) without it — the baseline that delta is measured against | 1,855,442 B (97.6 %) | 95,836 B |
| 2026-09-16 LVGL pool safety (one page resident at a time, the panel guard, the `/api/debug/page` hook and the pool fields on `/api/debug/ui`, the logged assert handler) | 1,847,562 B (97.2 %) | 95,732 B |
| 2026-09-16 screen pass (phone-style Wi-Fi bars, stats page with the on-time meter in three layouts, device page with SEPTA/SD health and the Data sources card, stats+device built on demand) on top of the exception pool and streamed `/api/state` | 1,840,258 B (96.8 %) | 95,652 B |
| Same tree without the screen pass (`next`, 2026-09-16) | 1,832,302 B (96.4 %) | 95,652 B |
| v0.2.0: the 2026-09-15 review fixes combined (PIN, Host check, OTA board check, WPA2 setup AP + QR, transport completeness, transit-first polling, per-stop health on the panels, checked SD writes, log export) | 1,828,384 B (96.2 %) | 95,644 B (29.2 %) |
| Hardening pass alone, with the QR widget compiled out | 1,767,926 B (93.0 %) | 95,580 B |
| Before the review fixes (v0.1.2) | 1,741,770 B (91.6 %) | 95,268 B |
| Everything incl. weather, Indego, profiles, night page, 48 px font | 1,858,446 B (97.8 %) | 95,300 B (29.1 %) |
| Same, before the second round of trims | 1,889,518 B (99.4 %) | |
| Weather only, before the first round | 1,897,974 B (99.9 %) | |

The two-reboot-paths pass is **-1,200 B of flash and +56 B of static RAM** on the 3.5" board,
leaving **38,802 B** of app slot - the first change in a while that gives flash back rather than
spending it, and it is worth saying where from, because two of the three parts were expected to
cost. Measured separately on the same tree: the watchdog half (`cpu_yield.*`, the yield in
`streamLogLines()`, the stats jobs answering through `sendJsonStreamed()` instead of building a
`String`, the `cpu_stretch_ms_max` field) is **-892 B**, because dropping the two
`serializeJson(doc, String&)` call sites removes more than the new header and the yielder add. The
middleware half (`HostGuardHandler`, `host_match.h`, the Host check and warming deleted from the OTA
callback) is a further **-704 B**: nothing constructs an `AsyncMiddlewareFunction` any more, so
`--gc-sections` drops `AsyncMiddlewareChain::addMiddleware`, `AsyncMiddlewareFunction::run` and its
destructors out of the image entirely, and `hostAllowed()` in fixed buffers is smaller than the same
work in `std::string`. The auth console lines are the only part that costs: **+396 B**, two format
strings and a `Serial.printf` per wrong PIN, which is the price of the only evidence that a lockout
happened (DESIGN.md §12.1).

Every env gives flash back by about the same amount, measured against the same `next` (baf20a0)
tree rather than against the table's older rows:

| env | `next` baf20a0 | with this pass | delta | slot left |
|---|---:|---:|---:|---:|
| `cyd-3248S035R` | 1,862,942 B | 1,861,742 B | -1,200 B | 38,802 B |
| `cyd-2432S024C` | 1,858,614 B | 1,857,538 B | -1,076 B | 43,006 B |
| `cyd-2432S028R` | 1,847,522 B | 1,846,342 B | -1,180 B | 54,202 B |
| `cyd-3248S035R-https` (§2.1, in no shipping image) | 1,881,530 B | 1,880,330 B | -1,200 B | 20,214 B |

Static RAM is +56 B on the three shipping envs and +32 B on the HTTPS prototype: the `g_host_name`
char array replacing a `std::string`, and one `volatile uint32_t` for the yield high-water.

The release-candidate review fixes are **+1,580 B of flash and +64 B of static RAM** on the 3.5"
board, leaving **41,110 B** of app slot. All six board envs build on the same tree:
`cyd-3248S035C` 1,871,646 B (98.5 %), `cyd-2432S028R` and `cyd-2432S024R` 1,844,034 B (97.0 %),
`cyd-2432S028Rv3` 1,842,978 B (97.0 %), `cyd-2432S024C` 1,855,226 B (97.6 %). The HTTPS prototype
`cyd-3248S035R-https` (§2.1; in no shipping image) is still the tightest env of all and still fits:
1,877,642 B (98.8 %), 22,902 B spare. Most of the delta is
comment-free code in four places - the `max()` and the per-row division in `main_screen.cpp`, the
OTA callback's split and its `bad_alloc` guard, `tryGetPollStatus()`, and the `lvgl_pool` branch on
`/api/state` with its `snprintf` format string. The liveness change itself is nearly free: two
`volatile uint32_t` stores and one `min()`, plus the `constexpr` arithmetic in
`poller_liveness.h`, which still inlines into one comparison. The two early exits on a transport
failure (`septa_source.cpp`, the BusSchedules wrapper) are a comparison each.

The poller-liveness net (`DESIGN.md` §12.1) is **+1,480 B of flash and +32 B of static RAM** on the
3.5" board, leaving 43,622 B of app slot. Most of it is the two `snprintf` format strings and the
`last_restart` object on `/api/state`; the threshold arithmetic itself is a `constexpr` header
(`src/app/poller_liveness.h`) that inlines into one comparison, and the RTC note is 20 B in
`.rtc_noinit` at `0x50000200`, which is not app-slot flash at all. The other two tight envs still
build on the same tree: `cyd-3248S035R-https` 1,875,114 B (98.7 %, the tightest) / 96,404 B, and
`cyd-2432S024C` 1,852,734 B (97.5 %) / 96,004 B.

The pool-safety pass is +6,172 B on the 3.5" board and +7,460 B on the 2.4" capacitive one, which
leaves 52,982 B and 57,226 B of app slot respectively. It would have been 1,256 B more: passing
`__func__` to `LV_ASSERT_HANDLER` gave a static name string to each of the ~400 LVGL functions the
macro expands in, so the handler prints its own return address instead and `addr2line` names it.
Every env still builds - `cyd-2432S028R` is 1,832,146 B and the HTTPS prototype
`cyd-3248S035R-https` 1,871,934 B (98.5 %, the tightest of the four).

The tightest board tracks it closely: `cyd-2432S024C` is 1,843,318 B (97.0 %) / 95,852 B after the
pool-safety pass, 1,835,858 B (96.6 %) / 95,788 B after the
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

Flash headroom is about 52 KB on the 3.5" boards and 56 KB on the 2.4" capacitive one.
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
(`transit_stats/tracker.h`, ~4 KB), the 3.5" draw buffer is 1/20 of the screen instead of 1/16
(board JSON, ~4 KB - and 1/30 since 0.3.1, another 5 KB), and the UI caches the visible-stop list
per build instead of copying the stop vector several times a second.

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

**Read the `free8` column.** It is the byte-addressable heap - what a buffer, a `std::string` or a
`JsonDocument` can actually be given. `free` is `ESP.getFreeHeap()`, kept here because it is what the
serial log and `/api/state`'s `heap` have always printed, but on this chip it also counts an IRAM
heap region that only 32-bit word access can reach, and `malloc()` never hands that out for data.

| After | `free8` (usable) | `free` (`ESP.getFreeHeap()`) | Largest block |
|---|---:|---:|---:|
| display (LVGL + 19 KB draw buffer) | 183 KB | 216 KB | 110 KB |
| config + arrival tracker (~12 KB) + poller task stack (10 KB) | 145 KB | 178 KB | 110 KB |
| Wi-Fi connected | 94 KB | 127 KB | 86 KB |
| web server, mDNS, SNTP | 67 KB | 100 KB | 61 KB |
| SD card mounted | 36 KB | 69 KB | 32 KB |
| UI screens built | 35 KB | 68 KB | 31 KB |
| steady state while polling (re-measured 2026-09-16) | ~39 KB | ~73 KB | ~20 KB |

The gap between the two columns is **33,708 B**, and it is a fixed region rather than a moving
figure: it is sized once when the app's IRAM code is placed and is never allocated from. Read from
paired measurements on the owner's cyd-3248S035R across two firmware builds and every point from
`display` to a poll in flight, and separately by a second agent across three boots, it comes back as
33,708 B. So the `free8` column above is the `free` column minus that constant. The largest-block
column was always `MALLOC_CAP_8BIT` and is unchanged.

One caveat on reading it out of `/api/state`: `heap` and `heap_8bit` are two counters sampled a few
microseconds apart inside one handler, `heap` first. An allocation landing between the two reads
lowers `heap_8bit` only, so the subtraction reads a few bytes **high** - 38 of 39 consecutive
samples gave 33,708 and one gave 33,736, at a mid-poll instant when another task was most likely
allocating. Every deviation seen has been positive, which is that artefact's signature and not a
region that varies. Treat a gap of 33,708 plus a few tens of bytes as the constant.

The boot rows are the historical `[heap]` readings restated; only the last row was re-measured on
2026-09-16, and it moved - it had said ~86 KB `free` with a ~43 KB block, which is higher than the
`UI screens built` row above it and did not match anything the board reports today. If the boot rows
are ever re-taken, take them from a single boot's `[heap]` lines, which now print `free8` directly
and need no arithmetic.

Measured on the owner's board at steady state on 2026-09-16: **39.8 KB of `free8` against 73.5 KB of
`free`**. Across 114 samples of ordinary polling (two stops, the owner's own config), `free8` ran
11.0-36.6 KB - median 34.9 KB, 5th percentile 17.4 KB - and the largest 8-bit block 8.7-20.5 KB,
median 20.5 KB. Both bottom out together, in the moments a poll is decoding a feed: that is the heap
genuinely being busy, and it is what the gates are there to wait out. A second agent sampling a
clean build with the same two-stop config got 36.8-39.3 KB of `free8` at idle, which agrees.

Sampling note, because it changes the numbers: only samples where a poll had already completed and
the device had not just rebooted are counted. Fresh-boot samples read ~48 KB of `free8` with a
47 KB largest block - the cleanest heap the device ever has - and including them inflates both the
ceiling and the apparent admission rates. The `[heap]` boot lines and the `[net_poller]` heartbeat
print both numbers (`free=` and `free8=`), and `/api/state` carries `heap` (unchanged, INTERNAL)
beside `heap_8bit` and `largest_block_8bit`.

**Expect `/api/state` to answer 503 for a few seconds during a firmware upload.** Measured on the
owner's board: fourteen `200`s then six `503 {"error":"low memory, retry"}` while a 1.85 MB image
was uploading, then recovery. An OTA holds a large sustained allocation and pushes the
byte-addressable heap under the 12 KB floor. This is new - the old gate's free half could not fire
at all - and it is deliberate: during an upload the status handler is competing with `Update` for
the last few KB, and a failed flash costs far more than a status reply that is briefly unavailable.
Treat 503 as "retry", which is what the low-memory contract has always meant. Note also that you
cannot measure the heap through `/api/state` at these moments - it refuses exactly when the number
you want is lowest - so use the `[net_poller]` serial heartbeat for an unperturbed reading.

Every heap gate in the firmware reads `MALLOC_CAP_8BIT` as of 2026-09-16, with thresholds re-derived
from what each path actually allocates (DESIGN.md §2.1): OTA needs 16 KB free and a 5,876 B block,
because `Update.begin()` allocates exactly one 4,096 B sector buffer; `/api/state` and `/api/config`
need 12 KB free and a 7,924 B block, against one 2,872 B send buffer and ArduinoJson's 1 KB pools;
the poller's idle slice needs 16 KB free and a 12,020 B block for its ~8 KB `StatsAggregator`.

**The block figures look arbitrary on purpose.** `heap_caps_get_largest_free_block()` returns values
on a 512-byte lattice at offset 500 (`500 + 512k` - all 24 distinct values measured here fit it
exactly), and **any multiple of 512 lands exactly 12 B above a lattice point, because 512 - 500 =
12**: a device resting there is refused by a hair while the next value up clears by 1,012 B. Note
this catches more than round kilobytes - 5,632, 7,680 and 11,776 are all multiples of 512 and all
equally bad. That is exactly how
the old 16,384 B OTA gate locked out a board resting at 16,372. 5,876 / 7,924 / 12,020 are
`756 + 512k`, i.e. mid-gap, 256 B from either neighbour. Do not "tidy" them to round kilobytes. Before that
they compared INTERNAL free against thresholds that only meant something in 8-bit terms, and two of
the three could not fire at all: INTERNAL free never drops below the 33,708 B of IRAM, so a 24 KB
`/api/state` floor and a 40 KB idle-work floor were unreachable, and those gates were running on
their largest-block halves alone.

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
- LVGL's static pool is 36 KB (`LV_MEM_SIZE`) and stays there — see `boards/README.md`; the draw
  buffer is 1/30 of the screen in RGB565 on the 3.5" boards (`LVGL_BUFFER_PIXELS` in
  `boards/*.json`). The `[lvmem]` lines show pool usage — at boot, and one per page built.
- **One page is resident at a time** (DESIGN.md §8). LVGL 9.5 does not survive running that pool
  out: `lv_obj_class.c` writes each new child straight after an unchecked `lv_realloc()`. So every
  page transition frees the page it is leaving before it builds the next one, the night page
  replaces the arrivals page rather than sitting beside it, and `createMainScreen()` stops adding
  stop panels while there is still room. Measured on the owner's board: the arrivals page is
  19,280 B with two stops and 31,656 B with four, of 36,864 B; stats 8,220 B, device info
  10,336 B, night ~1,740 B. A stop panel is ~6.2 KB on a 320-wide board and ~4.4 KB on a 240-tall
  one, so four stops is the ceiling on the 3.5" boards and six on the 2.4"/2.8" ones — the panel
  guard truncates the page and says so rather than crashing at boot on a config §6 accepts.
  `GET /api/debug/ui` reports `lv_free`, `lv_max_used`, `lv_page_cost` and `lv_page_refusals`;
  `POST /api/debug/page` (PIN-gated) drives the cycle so the pool can be watched under it.
- The ESP32's static `.bss` budget is separate from, and much smaller than, the heap: a ~14 KB
  object declared at file scope fails to link ("DRAM segment data does not fit").

### Verifying a change

`pio run -e cyd-3248S035R -t upload`, then watch the serial console for `[heap]`, `[lvmem]`,
`[net_poller] free_heap=... largest_block=...` and `[proxy]` lines while hitting the API from the
LAN (stop-list proxies for several routes, `/api/stats`, a CSV download). A healthy device shows
no `rst:` lines and a largest block above ~30 KB between polls.
