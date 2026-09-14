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

The app partition (`firmware/partitions.csv`) is 1,900,544 bytes (`0x1D0000`) per OTA slot.
Numbers below are from `pio run -e cyd-3248S035R`'s own size report, recorded after each
milestone of the SS13 feature work (all figures against the current, full feature set):

| Milestone | Flash (bytes) | Flash % | RAM (bytes) | RAM % |
|---|---:|---:|---:|---:|
| Skeleton (starting point) | 1,863,762 | 98.1% | 118,932 | 36.3% |
| Flash diet (drop WiFiManager, `CORE_DEBUG_LEVEL=2`, trim `lv_conf.h`) | 1,742,906 | 91.7% | 118,132 | 36.1% |
| 1. Serve embedded web UI | 1,762,270 | 92.7% | 118,132 | 36.1% |
| 2. Real SEPTA data (net_poller rewrite) | 1,830,438 | 96.3% | 118,268 | 36.1% |
| 3+4. Real `/api/state` + setup-wizard proxies | 1,837,098 | 96.7% | 118,260 | 36.1% |
| 5. Logging, stats, log downloads | 1,855,438 | 97.6% | 118,268 | 36.1% |
| 6. OTA (`POST /api/ota`) | 1,862,054 | 98.0% | 118,508 | 36.2% |
| 7. Real Snapshot on screen (main/stats UI) | 1,859,018 | 97.8% | 118,508 | 36.2% |
| 8. Brightness control | 1,859,126 | 97.8% | 118,508 | 36.2% |
| Fix: heap-allocate `/api/stats`' StatsAggregator (**final**) | **1,858,902** | **97.8%** | **118,508** | **36.2%** |

**Final headroom: 41,642 bytes (≈40.7 KB, 2.2%) — under the 64 KB target by about 23.9 KB.**
RAM is not a concern (36.2%, no changes needed there beyond heap-allocating the two objects
noted below).

The flash diet alone recovered 120,856 bytes (WiFiManager removal was the overwhelming majority
of that; see the "Flash diet" commit). Every feature milestone after it added real functionality
(transit_core's GTFS-RT decoder/SEPTA parsers, the stats aggregator, `Update.h`, the real UI) and
correspondingly cost flash; none of those increases reflect waste as far as I could find - see
"what I'd cut next" below for the concrete, measured options if getting under 64 KB matters more
than shipping all eight features in this pass.

**Two RAM (not flash) fixes were needed along the way**, both from declaring a "finished
library" object as a plain global rather than heap-allocating it: `transit_stats::ArrivalTracker`
(its own header docs put `sizeof()` at ~14-16 KB) and `transit_stats::StatsAggregator` (~8 KB)
both overflowed the ESP32's fixed `.bss`/`.data` budget ("DRAM segment data does not fit") when
declared as file-scope objects; both are now heap-allocated (`net_poller.cpp`) instead. This is
a general trap worth remembering: DESIGN.md's SS5 RAM budget is about the *heap*, and the two
libraries' own "long-lived singleton" framing invites a plain global, but the ESP32's static
budget is much smaller and separate from the heap.

### What I'd cut next, if 64 KB of headroom is required in this pass

Ranked by how much flash each measurably added (so, roughly, how much cutting it would give
back), from smallest/least-disruptive to largest/most-disruptive:

1. **Gate OTA behind an opt-in build flag** (`-DENABLE_OTA`, same pattern as `-DDEMO_DATA`),
   default off. Recovers roughly the milestone 6 delta, ~6.6 KB. The device is still fully
   flashable over USB (`docs/hardware.md` "Flashing") without it; OTA becomes something you
   turn on for a specific build rather than something every device carries by default.
2. **Gate the setup-wizard proxies** (`GET /api/proxy/stops`, `GET /api/proxy/schedule`,
   `proxy_worker.cpp`) the same way. Recovers most of the milestone 3+4 delta (~6-7 KB minus the
   trivial `/api/rail/stations` handler, which should stay). The web UI's Add Stop wizard would
   need a manual stop-ID entry fallback instead of the live SEPTA lookup/map flow (`web/README.md`
   already documents a manual-entry fallback for subway, so the wizard has precedent for this).
3. **Trade away `LV_DRAW_SW_COMPLEX`** (`lv_conf.h`) for `LV_DRAW_SW_COMPLEX 0`. I did not apply
   this: LVGL's own comment on the flag says `0` restricts drawing to "simple rectangles with
   gradient, images, texts, and straight lines only" - dropping rounded corners app-wide (every
   panel and route badge in `ui_common.cpp`/`main_screen.cpp` uses `lv_obj_set_style_radius()`),
   a visible design change I didn't think was mine to make silently. I don't have a measured
   number for this one; it's a real lever if the owner is fine with square corners.
4. **Defer `GET /api/stats`/on-device stats entirely** to a follow-up release. This is the
   single largest lever measured (milestone 5's +18.3 KB), and also the one I'd recommend against
   cutting: DESIGN.md SS8/SS9 treat stats as core to the product ("logs predictions... computes
   statistics"), not an add-on, and the SD logging itself (which has its own, smaller cost) would
   become pointless without a way to read it back.

None of these need touching `firmware/partitions.csv` (the task's hard constraint) - they're all
about which optional pieces of the DESIGN.md SS7 API surface ship in a given build.
