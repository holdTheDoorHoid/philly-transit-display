# Screenshot simulator (`pio run -e ui-sim`)

Renders the real LVGL screens from `src/app/ui/` on the host and writes PNGs, so a layout change
can be looked at on every board size and in both themes without a panel in front of you.
DESIGN.md SS8 lays everything out from the runtime resolution, which is exactly why "it looks fine
on my board" is not enough: 320x480 (the 3.5" boards, portrait), 480x320, 240x320 and 320x240 all
flow differently and the 2.4"/2.8" boards also use a different big-minutes font.

```sh
export PATH="$HOME/.platformio/penv/bin:$PATH"
cd firmware
pio run -e ui-sim
.pio/build/ui-sim/program ../docs/screens/after       # owner's board + 320x240, every page, both themes
.pio/build/ui-sim/program ../docs/screens/after full  # + 240x320, the four-stop config, stale header,
                                                      #   device page with everything failing, stats
                                                      #   with no on-time data
```

`docs/screens/before/` holds the v0.2.0 renders and `docs/screens/after/` the current ones, so a
layout change can be compared side by side; regenerate `after/` with `full` before committing a
screen change.

Each PNG is named `<page>-<WxH>-<theme>[-variant].png`. The program also prints, per
resolution, the LVGL pool usage after all four screens are built and the object count per screen -
the pool is 36 KB on the device (`include/lv_conf.h`), and every `lv_obj` costs memory. The host
figure is an over-estimate (64-bit pointers roughly double an `lv_obj`; measured 36,344 B on the
host against 23,556 B on the device for the same build, so multiply by about 0.65), and
`GET /api/debug/ui` on the device (`lv_used`/`lv_free`/`lv_max_used`) is the number that counts.

## What is real and what is faked

Real: `ui_common.cpp`, `main_screen.cpp`, `stats_screen.cpp`, `device_info_screen.cpp`,
`night_screen.cpp`, `demo_data.cpp`, the fonts and icons under `src/`, `include/lv_conf.h`, and
LVGL 9.5.0 from the registry - the same code and configuration the boards run. The transit_core,
transit_stats, daypart_core, indego_core and weather_core libraries are portable C++ and are
compiled as-is.

Faked (`stubs/` headers shadow the ESP32 ones because `-Isim/stubs` comes first on the include
path; `fakes.cpp` implements the app services the screens call):

| Screen calls | Sim answer |
|---|---|
| `WiFi.status()/RSSI()/SSID()/localIP()` | `sim::wifi_*` knobs (`stubs/WiFi.h`) |
| `millis()`, `delay()`, `ESP.getFreeHeap()`, `ESP.restart()`, `log_*` | `stubs/Arduino.h` - restart and Wi-Fi reset print a line and do nothing |
| `auth::pin()` | `123456` (the README's example, never a real device's PIN) |
| `getSdStatus()` | mounted, 3720 MB free (`sim::sd_mounted`), write health from `sim::sd_dropped_rows`/`sim::sd_error` |
| `getPollStatus()` | last SEPTA poll ok, 12 s ago (`sim::poll_ok`, `sim::poll_age_s`, `sim::poll_error`) |
| `getWeather()`, `getAlertsStatus()`, `getBikes().fetched_epoch` | feed ages for the Data sources card (`sim::weather_age_s`, `sim::alerts_age_s`, `sim::bike_age_s`; -1 = nothing fetched yet) |
| `getStopSummary(key)` | canned `StopSummaryView`s set per render (`sim::setSummary`); an unset key is "loading" |
| `getBikes()` | two Indego stations |
| `headerWeatherIcon()/Temp()/Text()`, `stopWeatherNote()` | `sim::weather_*`, `sim::stop_note` |
| `arrivalIsDue()`, `dueAlertTick()`, `dueChimesPlayed()` | never due |

The arrivals themselves are `demo_data.cpp`'s Snapshot, built relative to the wall clock, so the
minutes are always plausible; the clock in the header is the real local time (TZ is set to the
default `device.tz`).

## How it renders

`sim_main.cpp` creates one `lv_display_t` per resolution with a whole-frame RGB565 buffer in
`LV_DISPLAY_RENDER_MODE_DIRECT` (the flush callback only acknowledges), builds the four screens
from a `Config` like the owner's (two Route 17 stops, crowding words + icons, two Indego
stations) or a four-stop one, calls the same `refresh*Screen()` functions `ui.cpp`'s `tick()`
does, runs a few `lv_timer_handler()` frames so flex layouts and the ticker animation settle,
`lv_refr_now()`s, and writes the buffer as a PNG (zlib for deflate/crc32, hence `-lz`).

Two small hooks exist only for this build and change nothing on a board: `ui_common.cpp`'s
`fontBig()` reads `g_sim_small_board` under `UI_SIM` (both Montserrat 20 and 28 are linked on
the host, so the 240-tall boards' font can be shown per render), and `lv_conf.h`'s `LV_MEM_SIZE`
is `#ifndef`-guarded so the host can use a 512 KB pool.

`add_sim_sources.py` is a `pre:` extra script: `build_src_filter` cannot reach outside `src/`, and
the simulator must not live in `src/` where every ESP32 environment would compile it, so
`env.BuildSources()` adds `sim/*.cpp`. The library include paths are added there by hand because
the dependency finder only puts them on the project-source environment, not the one
`BuildSources` uses.
