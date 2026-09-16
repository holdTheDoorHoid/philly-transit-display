# Philly Transit Display — Design

Status: living document. Decisions here were made 2026-09-13 with the project owner and are
binding for contributors and coding agents unless this file is updated.

## 1. Goals

1. A desk display that shows the next 2-3 arrivals, per direction, for user-chosen SEPTA stops.
   The owner's first use case: Route 17 at 19th & Mifflin (southbound, stop 21332) and
   20th & Mifflin (northbound, stop 21297), both directions on one screen.
2. Shows lateness ("+13" late, "-1" early, "on time", or "sched" when no live vehicle), and
   SEPTA service alerts/detours for the configured routes.
3. Logs predictions and inferred actual arrivals to the SD card and computes statistics:
   lateness by hour and weekday, headway/bunching, ghost buses and dropped trips, and
   prediction accuracy. Stats are viewable in the device's web UI (charts) and in a summary
   page on the device screen.
4. Configurable entirely from a web page served by the device (no app, no cloud, no account).
5. Usable by anyone in Philadelphia: all SEPTA bus and trolley routes, Regional Rail stations,
   and every common CYD board variant. Subway (B/L lines) is schedule-only in v1 (see §4.6).
6. Portable beyond Philadelphia later: the real-time path is standard GTFS-Realtime.

Non-goals for v1: auto-dimming by light sensor, touch-driven multi-page stop groups, cloud
proxy, mobile app, BLE provisioning, NJ Transit (needs an API key each user must register for).

## 2. Decisions and rationale

| Decision | Choice | Why |
|---|---|---|
| Firmware stack | PlatformIO + Arduino-ESP32 core 3.x (pioarduino platform) + LVGL 9 + LovyanGFX via `rzeldent/esp32-smartdisplay` | Largest CYD community; one abstraction covers every CYD variant; owner chose it over Rust after research showed Xtensa Rust still needs a forked toolchain and has almost no CYD prior art. |
| Language | C++17 for firmware; vanilla HTML/JS/CSS for the web UI; small Python/Node scripts only for host tooling | Matches the ecosystem; no build step needed for the web UI beyond gzip embedding. |
| Real-time source (bus/trolley) | SEPTA GTFS-Realtime TripUpdates, stream-decoded on device, filtered to configured routes/stops | It is the only SEPTA feed with per-stop predicted arrival times. Its `trip_id` equals TransitView's `trip`, so lateness joins cleanly. Standard format = portability. Verified 2026-09-13 (§4.2). |
| Lateness source | TransitView `late` (minutes, server computed), joined on trip id | Authoritative; avoids guessing which scheduled trip a bus is. |
| Schedule source | BusSchedules per stop (cached 10 min) | Gives scheduled times for "sched" rows, no-show detection, and schedule matching. Must retry: endpoint intermittently returns 400/501 for valid stops. |
| Regional Rail | Arrivals API per station | Already includes `sched_time`, `depart_time`, and `status`; tiny payload. |
| Alerts | Alerts API with `routes=bus_route_17` style param | 1-3 KB per route. `req1=` silently returns `[]`; never use it. |
| Weather | Open-Meteo `/v1/forecast`, current + 6 hourly slots per stop location (§4.8) | No key, no account, answers over plain http:// (NWS is https-only, which this board cannot afford), ~900 B per request. One request per cluster of stops within 1.5 km, 10 min cadence. |
| Config storage | LittleFS `/config.json`, mirrored by `GET/PUT /api/config` | Structured, easy to round-trip to the web UI. SD is for logs only so the device works without a card. |
| Wi-Fi onboarding | Our own ~150-line captive portal (SoftAP + DNS hijack) on ESPAsyncWebServer; then always-on web UI + mDNS | WiFiManager was the first choice but cost ~120 KB of flash and spams `task_wdt` errors on core 3.x; replaced 2026-09-14. Captive portal is only for Wi-Fi; stop config lives in the permanent UI. |
| Web server | ESP32Async/ESPAsyncWebServer (maintained fork) | Chunked/streamed responses for stats and proxied SEPTA calls without blocking LVGL. Keep concurrent handlers minimal. |
| Transport | Plain HTTP only. The opt-in HTTPS mode (Amazon Trust CA bundle, `tls_verify`) shipped in v0.1.0-0.1.1 and was removed in v0.1.2 to free ~100 KB of flash; the board could not afford a TLS session's RAM anyway (section 2), and SEPTA, Open-Meteo and Bicycle Transit all serve plain http. |
| Log format | Append-only CSV, one file per month on SD, one event per line | Human-readable, spreadsheet-friendly, streamable with tiny RAM. |
| Stats computation | On device, streaming over CSV with fixed-size histograms | 320 KB RAM, no PSRAM; never load a month of log into memory. |
| Repo/License | `holdTheDoorHoid/philly-transit-display`, MIT | Owner's choice. |

## 3. Hardware

Target family: Sunton ESP32 "Cheap Yellow Display" boards. Classic ESP32-WROOM-32, 4 MB flash,
**no PSRAM**, ~320 KB DRAM of which roughly 100-160 KB is usable heap once Wi-Fi and one TLS
session are live. Every design choice must respect this.

Supported PlatformIO environments (board JSONs from `rzeldent/platformio-espressif32-sunton`):

| env | Board | Panel | Touch | Notes |
|---|---|---|---|---|
| `cyd-3248S035R` (default) | ESP32-3248S035R | 3.5" ST7796 480x320 | XPT2046 resistive | Owner's unit is believed to be a 3.5" (silkscreen "ESP32-035"). Backlight GPIO 27. |
| `cyd-3248S035C` | ESP32-3248S035C | 3.5" ST7796 480x320 | GT911 capacitive (I2C 21/22) | Probe decides R vs C. |
| `cyd-2432S028R` | ESP32-2432S028R | 2.8" ILI9341 320x240 | XPT2046 | Single micro-USB. |
| `cyd-2432S028Rv3` | 2-USB 2.8" | ST7789 320x240 | XPT2046 (X mirrored) | Different inversion/RGB order. |
| `cyd-2432S024R` / `C` | ESP32-2432S024 | 2.4" 320x240 | XPT2046 / CST816S | Pins differ; lower priority. |
| `native` | host | - | - | Unit tests for `transit_core` and `transit_stats`. |

SD card: SPI (CS 5, MOSI 23, MISO 19, SCK 18 on the 2.8"; confirm for 3.5"). RGB LED pins 4/16/17
(active low) can show status: blue = connecting, green blink = poll ok, red = error.

The UI must lay out from the runtime display resolution, not hardcoded 480x320.

## 4. Data sources (SEPTA)

All verified live on 2026-09-13; details and byte counts in `docs/research/septa-data-sources.md`.
No API keys. HTTPS with Amazon Trust Services roots. No documented rate limit: poll politely.

### 4.1 Route IDs are strings
SEPTA's 2026 bus network uses letter routes (`T4`, `G1`, `D1`, `K`, `BLVDDIR`, ...). Never parse
routes as integers. Trolleys are ordinary routes in the bus feeds; the alert prefix differs
(`trolley_route_T4` vs `bus_route_17` — verify prefix list from `Alerts/index.php` without params).

### 4.2 GTFS-RT TripUpdates (primary real-time source)
`https://www3.septa.org/gtfsrt/septa-pa-us/Trip/rtTripUpdates.pb`, ~150 KB, ~320 entities,
max entity 2.3 KB. Each entity: `trip_update { trip { trip_id, route_id, direction_id },
vehicle { id }, stop_time_update[] { stop_sequence, stop_id, arrival { time }, schedule_relationship } }`.
Verified: for route 17, entity `3667` had `arrival.time` for stop `21332` at sequence 35, and
`trip_id` 3667 == TransitView `trip` 3667 for vehicle 7477 with `late: 13`.

Device behaviour: stream the HTTP body; parse the top-level `FeedMessage` incrementally; for
each `entity` (field 2, length-delimited) buffer at most `GTFSRT_MAX_ENTITY` (4096) bytes, skip
larger ones; decode only if `trip.route_id` is in the configured set; emit stop_time_updates for
configured stop_ids. Stop sequences let us know when a bus has passed our stop. Schedule
relationship `SKIPPED` (1) on our stop means detour — show it, including when the update carries no
time at all (it is then shown at the scheduled time it matches, and that schedule row is consumed
so the same trip is not also advertised as running). The **trip-level** `schedule_relationship` in
`TripDescriptor` is a different enum with overlapping numbers: `CANCELED` (3) there means the trip
is not running, so it is shown as nothing at all and suppresses its matching scheduled row.
`NO_DATA` (2) at the stop level is not a prediction — no time is invented for it, and the schedule
row stands in for that trip.

The feed's `FeedHeader.timestamp` is carried through to every decoded update and on to
`StopSnapshot.source_ts` (§7): a cached or replayed body is well-formed and full of future
predictions, so the header timestamp is the only thing that distinguishes it from a live one.

Rail RT feeds (`septarail-pa-us/...`) are 3-4 KB but the Arrivals JSON API is richer; not used in v1.

### 4.3 TransitView per route
`.../api/TransitView/index.php?route=17` (1-3 KB). Fields used: `trip`, `VehicleID`, `late`,
`destination`, `Direction`, `next_stop_sequence`, `estimated_seat_availability`, `timestamp`.
Never use `TransitViewAll` (129 KB).

### 4.4 BusSchedules per stop
`.../api/BusSchedules/index.php?stop_id=21332` (~700 B). Returns `{ "<route>": [ {trip_id,
DateCalender "09/13/26 10:25 pm", Direction "0"/"1", DirectionDesc} ] }`. Scheduled only. Its
`trip_id` is the static-GTFS id and does NOT match real-time trip ids. Cache 10 minutes.
Retry up to 3 times with backoff on any non-200 (observed 400 and 501 for valid stops). **Note:** a
501 can carry a perfectly valid JSON body, so parse whatever body arrives and treat only the
`{"error": ...}` shape as a real failure (verified 2026-09-13, see `firmware/lib/transit_core/NOTES.md`).

**Wrong service day (verified 2026-09-14 09:15-09:24):** the endpoint is served by more than one
backend and they disagree. Five identical requests for stop 21297 returned today's 9:30 am trips
three times and *tomorrow's* 12:32 am owl trips twice (the latter under the pre-Bus-Revolution
"Front-Market" headsign, so it is a stale schedule with no service for the current day); stop 21332
did the same. Displayed naively that is "887 minutes". `fetchPlausibleSchedule()` refetches up to 3
times while the earliest upcoming entry is more than 2 h away, keeps the answer whose first trip is
soonest, and the firmware caches a still-implausible answer for only 2 minutes instead of 10
(`ScheduleCache::putSuspect`). `mergeStop()` also collapses duplicate static trip ids (the bad
backend has listed one trip on three consecutive days). Regardless of source, the UI shows any
arrival an hour or more away as a clock time ("1:14a") rather than a minute count (§8).

The endpoint is an AWS load balancer over at least six backends (`X-B-Srvr: api_main2`…`6`); on
2026-09-14 `api_main3` answered for today while `api_main5` and `api_main6` each held a different
stale schedule, and every new TCP connection lands on a random one. The balancer issues a sticky
`AWSELB` cookie (10 min max-age, refreshed per request), so `http_fetch.cpp` keeps the cookie from
the last backend that answered for today and sends it with every BusSchedules request
(`pinScheduleBackend`), dropping it after a wrong-day answer so the next connection can land
elsewhere. Each fetch logs `[net_poller] BusSchedules stop N: backend api_mainX, first trip in M
min`.

### 4.5 Stops per route (setup only)
`.../api/Stops/index.php?req1=17` (12 KB, 133 stops, in route order but both directions in one
list). Proxied to the browser during setup; streamed, never buffered whole.

### 4.6 Regional Rail and Subway
Rail: `.../api/Arrivals/index.php?station=<name>&results=5&direction=N|S`. "Northbound/Southbound"
are legacy division names, not compass. Line/destination filtering is client side. Station names
accepted by the API differ from GTFS `stops.txt` for 13 stations; `rail_stations.cpp` carries the
corrected list (149 verified names). Six stations could not be resolved and are excluded for now:
Fern Rock, Holmesburg Junction, Norristown Transit Center, Richard Allen Ln, Airport Terminals C&D,
Delaware Valley University.
Subway (B and L lines): `TransitView?route=BSL` and `Stops?req1=BSL` return `[]`, and the bus
GTFS-RT feed carries no subway trips (verified). **Verified 2026-09-13:** BusSchedules does serve
subway station stop_ids, keyed by the GTFS route ids `B1` (Broad Street) and `L1` (Market-Frankford),
not `BSL`/`MFL` (fixture `busschedules_bsl_1286.json`). So v1 shows subway stops schedule-only.
`mergeStop` maps between the two id schemes explicitly (`schedRouteMatches()`, `merge.h`): bus and
trolley require an exact route match, because a shared stop's `BusSchedules` response carries every
route serving it and matching on direction alone put route 2's departures under route 17's panel;
`Mode::Subway` accepts the configured id plus its known GTFS ids (`BSL` → `B1`/`B2`/`B3`, `MFL` →
`L1`/`L2`), and — only for subway, whose station stop_ids serve one line — accepts anything for a
line whose GTFS ids this project has not verified. The UI labels
such rows `sched` and the stop panel says "schedule only" for subway.
Alerts for subway and Regional Rail use the `rr_route_` prefix (`rr_route_bsl`, `rr_route_mfl`,
`rr_route_trent`); Regional Rail line codes need a lookup table, not a formula. Bus and trolley
alerts use `bus_route_<id>` including the T, G, D, and K letter routes.

### 4.7 Poll schedule (defaults, configurable)
| What | Interval | Notes |
|---|---|---|
| GTFS-RT TripUpdates | 30 s (15 s when an arrival is < 3 min out) | One TLS session; close after. |
| TransitView per configured route | same cycle, right after TripUpdates | Reuse the connection if keep-alive works. |
| BusSchedules per stop | 10 min | Also on config change. Buffered (~1 KB) and retried up to 4 times on SEPTA's `{"error"}` body. |
| Alerts per route | 5 min | |
| Rail Arrivals per station | 30 s | |
Open, GET, parse, close. Never hold a TLS socket across the idle gap. Back off exponentially on
failure (30 s → 60 s → 120 s → cap 5 min) and show "stale N min" on screen.

**Transit first (2026-09-15).** TripUpdates + TransitView + BusSchedules are the *only* fetches on
the path between SEPTA and the screen. The poller publishes the `Snapshot` — stamping `generated`
at that moment — the instant the stops have merged, and *then* runs alerts, weather, Indego, route
liveness and the SD log, each on its own due time. Before this, all of those sat in front of the
publish, so a slow alerts endpoint and a 400 KB bike feed could put twelve seconds between the
arrivals being fetched and their appearing, with `generated` claiming they were fresh. Alerts
belong to the same `Snapshot`, so a cycle that fetches new ones publishes a *second* time (same
stops, plus alerts) rather than withholding the arrivals until they arrive. Optional work is
skipped entirely when the next transit poll is less than 5 s away, uses a shorter per-request
timeout than the transit fetches, and checks the remaining budget between items.

**Request deadlines.** Every request carries an absolute deadline of 2× its timeout, capped at
30 s, enforced inside the body sink: HTTP client timeouts are per *read*, so a peer trickling one
byte per window can hold the poller task — and therefore the display — indefinitely. A body cut
off by that deadline is reported *incomplete*, never as a short success. Completeness is reported
by the transport, not guessed: a connection dropped halfway through the 150 KB TripUpdates feed
still reports HTTP 200, and the stops the missing half would have filled then look empty and
successful. See `transit::FetchResult` / `HttpGetEx` in `transit_core/source.h`.

### 4.8 Weather (Open-Meteo)
`http://api.open-meteo.com/v1/forecast?latitude=39.9279&longitude=-75.1771&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m&hourly=weather_code,precipitation_probability,temperature_2m&forecast_hours=6&timezone=auto&wind_speed_unit=mph&temperature_unit=fahrenheit`
(~900 B, verified 2026-09-14 over plain HTTP, no redirect). `current.weather_code` is a WMO
interpretation code; `hourly.*` are six arrays starting at the current hour. `lib/weather_core`
parses it and maps codes to words ("mostly clear", "light rain", "thunderstorm"). Stops carry
`lat`/`lng` from the Stops API (§4.5); stops within 1.5 km share one request, at most four
locations, refreshed every 10 minutes and on config change. The first configured stop's location
is the "main" one the header shows as a colour condition icon plus the temperature (`[sun] 69°`;
  the words only while there is no forecast). A stop panel gets a one-line note about
the hour of its next arrival ("light rain at 10:15a, 62°", "rain likely (55%) at 10:15a") only
when that hour is *notable* next to the header: precipitation probability ≥ 40 %, any wet weather,
or a different kind of weather (fog vs clear); clear-vs-cloudy is not worth a line. Attribution:
"Weather data by Open-Meteo.com" (CC BY 4.0) in the web UI and README.

### 4.9 Indego bike share (Bicycle Transit status feed)
`http://bts-status.bicycletransit.workers.dev/phl` (verified 2026-09-14: plain HTTP, no redirect,
CORS `*`, `Cache-Control: max-age=60`, ~400 KB GeoJSON, 319 stations). The official GBFS feed at
`gbfs.bcycle.com` is https-only and returns 403 over http, so it is not usable from the device.
Each feature is `{ "geometry": {"coordinates": [lng, lat]}, "properties": { "id": 3468, "name":
"Snyder & Dorrance", "totalDocks", "docksAvailable", "bikesAvailable", "classicBikesAvailable",
"electricBikesAvailable", "kioskPublicStatus": "Active", ..., "bikes": [ per-dock entries ] } }`;
a feature with its dock list can run to ~3 KB. The device never buffers the body: `lib/indego_core`
scans the stream feature by feature (brace depth inside `"features"`), keeps at most one feature
(cap 6 KB) and parses only those whose `"id"` is configured. The web UI fetches the same feed in
the browser to offer the stations nearest each configured stop. Cadence 5 min.

## 5. Firmware architecture

```
firmware/
  platformio.ini                 envs listed in §3; common build flags; lib_deps pinned
  boards/                        sunton board JSONs (vendored, with source URL and commit)
  src/main.cpp                   setup(): board init, LittleFS, config, WiFiManager, NTP, mDNS, web, tasks
  src/app/                       glue that is Arduino-specific:
    net_poller.{h,cpp}           FreeRTOS task on core 0: runs the §4.7 schedule, produces Snapshot
    http_fetch.{h,cpp}           plain-HTTP GET, retry/backoff, streaming callback, sticky BusSchedules cookie
    config_store.{h,cpp}         LittleFS <-> Config struct, validation, defaults, migration
    web_server.{h,cpp}           routes in §7, serves gzipped assets from src/generated/
    ui/                          LVGL screens: main, stats, device info; uses only Snapshot data
    sd_logger.{h,cpp}            SD mount, monthly CSV append, disk-full handling
    status_led.{h,cpp}
  src/generated/web_assets.h     produced by web/build.mjs (committed; CI verifies freshness)
  lib/transit_core/              Arduino-independent, tested on host
    include/transit_core/model.h        shared structs (this file is authoritative)
    include/transit_core/gtfsrt_stream.h streaming protobuf decoder with route/stop filter
    include/transit_core/septa.h         parsers: TransitView, BusSchedules, Alerts, Arrivals, Stops
    include/transit_core/merge.h         combine RT + TransitView + schedule -> StopSnapshot
    include/transit_core/timeparse.h     "09/13/26 10:25 pm" and "2026-09-13 22:24:00.000" in America/New_York -> epoch
  lib/transit_stats/             Arduino-independent, tested on host
    include/transit_stats/events.h       LogEvent struct <-> CSV line
    include/transit_stats/tracker.h      ArrivalTracker: StopSnapshot stream -> LogEvents
    include/transit_stats/aggregate.h    StatsAggregator: CSV lines -> fixed-size histograms -> JSON
  test/                          PlatformIO Unity tests, run with `pio test -e native`
  test/fixtures/                 real SEPTA responses captured 2026-09-13
```

Tasks and cores: LVGL tick/handler runs on core 1 with a 5 ms cadence and must never block on
network or SD. Network polling runs on core 0, and the same task drains the web server's deferred
job queue (setup-wizard proxies, statistics) between polls; there is no separate worker task because
its stack did not fit (see `firmware/README.md`, Memory and flash budget). Shared state is a `Snapshot` guarded by a mutex,
swapped whole (never mutated in place). SD writes happen from a low-priority logger task fed by a
FreeRTOS queue. Web handlers only read the Snapshot and the config; stats requests stream the CSV
through the aggregator inside the handler in chunks small enough to keep the heap flat.

**Nothing on the LVGL task may touch SD or the network** (2026-09-15). That includes the stats
page: `getStopSummary()` is non-blocking and returns the cached summary with its age plus a
"nothing computed yet" flag, and the poller task recomputes summaries in its idle slices — one
stop per slice, at most every 10 minutes per stop or on request, evicting stops that are no longer
configured. It used to stream a month of CSV per stop synchronously on whichever task asked, so
opening the stats page froze touch and the clock for as long as the card took.

**The idle loop runs one deferred job per slice** and re-checks the poll deadline afterwards.
Draining the whole queue back to back (a 400 KB stop-list proxy and a 30-day stats scan are each
seconds of work) pushed the next transit poll well past its deadline with nothing noticing.

**The two LittleFS proxy temp files are leased**, not alternated. A file is held for the whole
life of the response that serves it and released by the request's disconnect callback (which fires
on both completion and abort); with both leased a new job answers `503 {"error":"busy, try
again"}`. Alternating an index only works for exactly two requests in flight: a third rewrote a
file that was still being streamed, and the client got two half-responses spliced together under
a 200.

**The SD mount allows two open files**, and the poller holds one whenever it appends a row or
scans for a summary, so downloads take a single-reader lease (`acquireLogReader()`); a second
concurrent download gets a 503 rather than a truncated file.

Memory rules: no full framebuffer; LVGL partial buffer is 1/10 of the screen in RGB565 (the library default of 1/4 with 3-byte pixels does not fit, see `firmware/boards/README.md`); large long-lived objects (ArrivalTracker ~16 KB, StatsAggregator ~8 KB) are heap-allocated, never file-scope globals, because the ESP32's static .bss budget is separate from and much smaller than the heap; one
TLS connection at a time; ArduinoJson documents sized from measured payloads (§4) with 25 %
headroom; log free heap once per poll at `INFO`; refuse to start OTA if free heap < 60 KB.

Flash budget: the app slot is 1,900,544 bytes. As of 2026-09-15, with the §12 hardening, the full feature set uses 93.9 % on `cyd-3248S035R` and 93.7 % on the tightest board, `cyd-2432S024C` (~112 KB headroom); `firmware/README.md` ranks what to cut if more is needed — the largest single item is the setup screen's QR code at 17 KB. Do not grow the app slots without dropping OTA.

Build/flash: `pio run -e cyd-3248S035R`, `pio run -e cyd-3248S035R -t upload --upload-port
/dev/ttyUSB0`. Releases publish `bootloader.bin`, `partitions.bin`, `firmware.bin` per env plus an
ESP Web Tools `manifest.json` under `flasher/` for GitHub Pages (offsets 0x1000 / 0x8000 / 0x10000).

## 6. Configuration schema (`/config.json`, version 1)

```json
{
  "version": 1,
  "device": {
    "name": "transit-display",
    "tz": "EST5EDT,M3.2.0,M11.1.0",
    "poll_seconds": 30,
    "brightness": 80,
    "rotation": 0,
    "theme": "light",
    "invert_colors": true,
    "ticker_lines": 3,
    "ticker_speed": 30,
    "logging": true,
    "header": { "name": false, "clock": true, "weather": true, "wifi": true, "updated": true },
    "large_text": false,
    "crowding": "words",
    "crowding_icons": "seats",
    "quiet": { "enabled": false, "start": "23:00", "end": "06:00", "brightness": 0, "wake_seconds": 30 },
    "night": { "enabled": true, "after_min": 60 }
  },
  "stops": [
    {
      "key": "17-21332",
      "mode": "bus",
      "route": "17",
      "stop_id": "21332",
      "direction": "1",
      "headsign": "20th-Johnston",
      "label": "17 Southbound",
      "stop_name": "19th St & Mifflin St",
      "show": 3,
      "lat": 39.927947,
      "lng": -75.177147,
      "title_style": "label_dest",
      "title_text": "",
      "alt_of": "",
      "alt_after_min": 15
    },
    {
      "key": "17-21297",
      "mode": "bus",
      "route": "17",
      "stop_id": "21297",
      "direction": "0",
      "headsign": "2nd-Market",
      "label": "17 Northbound",
      "stop_name": "20th St & Mifflin St",
      "show": 3
    },
    {
      "key": "rail-30th-N",
      "mode": "rail",
      "station": "30th Street Station",
      "direction": "N",
      "line": "",   // maps to StopConfig::route for rail stops (optional line filter)
      "label": "Regional Rail North",
      "show": 2
    }
  ],
  "alerts": true,
  "weather": { "enabled": true, "per_stop": true, "units": "f" },
  "due": { "enabled": true, "minutes": 3, "led": true, "screen": true, "chime": false },
  "profiles": [
    { "name": "Weekday morning", "days": [1, 2, 3, 4, 5], "start": "05:30", "end": "10:00",
      "stops": ["17-21297", "17-21332"] }
  ],
  "bike": { "enabled": false, "stations": [ { "id": 3468, "name": "Snyder & Dorrance" } ] }
}
```

Fields added 2026-09-14 (all optional; absent means the default shown above):
- `stops[].title_style`: how the panel is titled on screen. `label_dest` (default) = `17 Southbound → 20th-Johnston`
  (label falls back to the route); `label` = the label alone; `route_dest_stop` = `17 → 20th-Johnston • 19th St & Mifflin St`;
  `custom` = `title_text` verbatim. Rail: `label_dest` = `Label (Northbound)`.
- `stops[].alt_of` + `alt_after_min`: this stop is an *alternative* shown only while the stop whose
  key is `alt_of` has no arrival within `alt_after_min` minutes (or no data). Otherwise its panel is
  hidden and the others take the space. Alternatives still poll and log normally.
- `device.large_text`: two rows per stop with the minutes in a 48 px digits font (readable across a
  room). `device.crowding`: SEPTA's estimated seat availability next to the destination as
  `off`, `words` (`open`, `few seats`, `standing`, `packed`, `full`, and SEPTA's rare `empty`),
  `icons`, or `both`. `device.crowding_icons` picks the icon meter: `seats` (three slots that fill
  with green chairs while you can sit: 3 empty / 2 open / 1 amber few seats; then people once you
  stand: 1 amber standing / 2 red packed / 3 red full) or `crowd` (people only, 1 green / 2 amber /
  3 red, more people = more crowded). Unused slots stay dim so the meter keeps its width. The
  glyphs are a 16 px chair + person subset of FontAwesome (`src/fonts/`, ~1.5 KB) coloured per slot
  with LVGL's inline recolor. v0.1.0 stored a boolean `show_crowding`; it is still read (false ->
  `off`) but no longer written.
- `device.quiet`: between `start` and `end` (local, may cross midnight) the backlight drops to
  `brightness` percent (0 = off); a touch restores it for `wake_seconds` and does not change page.
  Chimes are suppressed in quiet hours.
- `device.night`: when no configured stop has an arrival within `after_min` minutes the main page
  becomes a clock: big time, date, weather, and each stop's next departure ("17 Southbound 5:12a").
- `due`: when an arrival first comes within `minutes`: the RGB LED blinks green (`led`), the row's
  minutes blink (`screen`), and the speaker plays two short beeps once per trip (`chime`; GPIO 26 on
  the Sunton boards). Off in quiet hours.
- `profiles` (max 4): while a profile is active (`days` 0=Sunday..6, `start`/`end` local), the main
  page shows only its `stops` in that order; otherwise all stops in config order. Every stop keeps
  polling and logging.
- `bike` (max 3 stations): Indego bikes/docks from Bicycle Transit's status feed (SS4.9). `style`
  is `icons` (default: bicycle = classic bikes, bolt = e-bikes, P = free docks, each count red at 0
  and amber at 1-2) or `words` (`5 bikes, 2 e-bikes, 7 docks`, same colour cue on the numbers).
  `stations[]` carry the BTS station `id` and a display `name`.

`rotation` is 0, 90, 180, or 270 degrees; 0 is the panel's native portrait orientation (the owner's preference), 90 is landscape. The UI rebuilds its layout when it changes.
`theme` is `light` (default) or `dark`; both palettes keep every text colour at WCAG AA contrast or
better. `invert_colors` drives the panel controller's colour-inversion command; its default comes
from the board build flag `DISPLAY_INVERT_DEFAULT`, off for every vendored board (`docs/hardware.md`
"Colour inversion" records the false alarm that briefly turned it on for the 3.5" boards).
`ticker_show` picks what the alert ticker carries: `both` (default), `alerts`, `detours`, or `off`
(no ticker at all; `alerts` at the top level still decides whether alerts are fetched). `ticker_lines` (1-8) is the height
of the alert ticker in text lines: 1 is a sideways marquee, more wraps the text and scrolls it
upward. `ticker_speed` (5-200) is the scroll speed in pixels per second. All four apply without a
reboot.
`header` picks what the main screen's top strip shows (§8); the device name is off by default
because the strip is narrow. `stops[].lat`/`lng` come from the Stops API when a stop is added
(the two default stops are backfilled on load; the web UI's Stops page offers a lookup for older
entries). `weather` (§4.8): `enabled`, `per_stop` (the per-panel notes), `units` `f`|`c`.
`mode` is one of `bus`, `trolley`, `subway`, `rail`. `key` is generated by the UI and must be
unique and stable; it is the join key in the log. Maximum 8 stops. Validation errors return
HTTP 400 with `{ "error": "...", "path": "stops[1].stop_id" }`.

### 6.1 Validation limits

`jsonToConfig()` in `config_store.cpp` is the only door configuration comes through, and it
checks *before* it narrows: numbers are read as int64/double and range-checked against the ranges
below, so `brightness: 256` and `poll_seconds: 65566` are 400s rather than silently wrapping to 0
and 30 in their `uint8_t`/`uint16_t` fields. The first violation decides the response; its JSON
path is returned.

- Numbers: `poll_seconds` 5–600, `brightness` 0–100, `rotation` 0/90/180/270, `ticker_lines` 1–8,
  `ticker_speed` 5–200, `quiet.brightness` 0–50, `quiet.wake_seconds` 5–300, `night.after_min`
  15–240, `due.minutes` 1–15, `stops[].show` 1–4, `stops[].alt_after_min` 5–60 (when `alt_of` is
  set), `bike.stations[].id` ≥ 1. `lat`/`lng` must be finite and within ±90 / ±180.
- Strings: `device.name` ≤ 32 and, after lower-casing ("slugifying"), only `[a-z0-9-]` and no
  leading or trailing `-` — it is the mDNS hostname, and a character DNS cannot carry is refused
  rather than guessed at; `device.tz` ≤ 64; every per-stop string (`key`, `route`, `stop_id`,
  `direction`, `headsign`, `label`, `stop_name`, `station`, `title_text`, `alt_of`) ≤ 64; profile
  names ≤ 32; bike station names ≤ 48. No string may contain control characters.
- Arrays, capped while being read rather than after: `stops` ≤ 8, `profiles` ≤ 4, a profile's
  `stops` ≤ 8 with unique keys that all exist in `stops`, `bike.stations` ≤ 3.
- Wrong JSON types are 400s, not silent fallbacks, for every string and number. Booleans are the
  exception: a wrongly-typed boolean falls back to its documented default, which cannot wrap,
  truncate or grow anything.

**Regional Rail line normalisation.** A rail stop's line may be sent as the code (`"PAO"`) or as
the display name SEPTA's own responses use (`"Paoli/Thorndale"`); only the code matches anything
downstream, so a display name is converted to its code on the way in (case-insensitively, against
`transit::kRailLines`). An unrecognised line is a 400 at `stops[i].route`: *"unknown Regional Rail
line; use a line code such as PAO"*. Bus, trolley and subway route ids are SEPTA's own strings and
are stored exactly as sent.

### 6.2 Durable save

A save never leaves the live file half-written. `saveConfig()` serializes to `/config.json.tmp`,
checks the byte count written equals the serialized length (a short write is what a full LittleFS
looks like from the File API), re-reads that file and compares its length and FNV-1a hash with the serialization (a byte-identical copy of an already-validated config needs no second parse, which cost ~15 KB of heap on the web task), rotates the current
`/config.json` to `/config.prev.json`, and only then renames the temp file into place. Config
writes are serialized with a mutex — `PUT /api/config` runs on the web server's task while
`main.cpp` may still be writing defaults. Any failure returns false, `PUT /api/config` answers 500
and the running config is untouched.

`loadConfig()` tries `/config.json`, then `/config.prev.json`, logs which one it used, and reports
a fallback as `config_recovered: true` in `GET /api/state` so the owner learns a save did not
survive instead of noticing a reverted setting days later.

## 7. Device HTTP API

All JSON. **Viewing is open; changing needs the PIN.** Any client on the LAN can read state,
config, stats, the log index and the proxies — the display behaves like an appliance and the web
UI needs no login to show arrivals. Everything that changes the device carries the per-device
admin PIN in an `X-Pin: <pin>` header (§12; the PIN is printed on the serial console at boot and
shown on the device info screen). CORS is deliberately not enabled and the UI is same-origin, so a
cross-origin page cannot attach that header without a preflight this server never answers.

Protected: `PUT /api/config`, `POST /api/reboot`, `POST /api/wifi/reset`, `POST /api/ota`,
`POST /api/pin`, `POST /api/debug/tap`, `POST /api/debug/oom`, `GET /api/log/<file>.csv`.
Open: everything else, including `GET /api/log/index` and `GET /api/debug/ui`.

Status codes beyond the per-route ones below:

| Code | Body | When |
|---|---|---|
| 401 | `{"error":"pin required"}` / `{"error":"wrong pin"}` | Protected route, `X-Pin` missing or wrong |
| 429 | `{"error":"too many attempts","retry_s":N}` | Five consecutive wrong PINs; every protected route is locked for 30 s. A correct PIN resets the counter; a *missing* header never counts towards it |
| 421 | `{"error":"this device is not reachable under that host name"}` | The `Host` header is not the device's IP, `<device name>` or `<device name>.local`, `192.168.4.1` or `localhost` (optional `:port`, case-insensitive). DNS-rebinding defence — checked before any handler runs, on every route |
| 409 | `{"error":"another firmware upload is in progress"}` | A second `POST /api/ota` while one is streaming |
| 500 | `{"error":"..."}` | `PUT /api/config` could not write the file (the live config is unchanged, §6) |

`GET /` additionally answers with `X-Frame-Options: DENY`, `Content-Security-Policy:
frame-ancestors 'none'` and `X-Content-Type-Options: nosniff`. There is no `script-src` directive:
the Stops page loads Leaflet from a CDN (§10).

| Method, path | Purpose |
|---|---|
| `GET /` , `/app.js`, `/app.css`, `/favicon.svg` | Web UI, served gzip with `Cache-Control: max-age=3600`, ETag = firmware build id |
| `GET /api/state` | Current snapshot: time, uptime, heap, wifi {ssid, rssi, ip, mdns}, sd {mounted, free_mb, log_bytes, dropped_rows, write_ok, error} (§9.1), last_poll {ok, age_s, error}, `stops[]` each with `arrivals[]` (§8 shape), `ok`, `health`, `source_ts`, `source_age_s` (-1 when the feed carried no timestamp) and `weather_note`, `alerts[]`, `weather {enabled, units, age_s, stale, main {temp, feels_like, code, text, wind, hours[]}}` (§4.8; `age_s` is the main location's last successful fetch, `stale` once that is over an hour old), plus `board` (the PlatformIO env this image was built for, e.g. `cyd-3248S035R`), `auth {pin_required}` and `config_recovered` (§6) |
| `GET /api/config` | Current config (§6) |
| `PUT /api/config` | Replace config; validates; persists; triggers immediate re-poll. 400 on error |
| `GET /api/proxy/stops?route=17` | Streams SEPTA `Stops` for a route to the browser (setup only) |
| `GET /api/proxy/schedule?stop_id=21332` | Proxies `BusSchedules` with retries; used to learn directions served at a stop |
| `GET /api/rail/stations` | Static Regional Rail station list embedded in firmware |
| `GET /api/stats?stop=<key>&days=30` | Aggregates (§9.3) computed by streaming the log |
| `GET /api/stats/overview?days=7` | Every stop and Indego station in one pass (§9.3), for the Stats page's comparison table |
| `GET /api/log/index` | `[ { "file": "2026-09.csv", "bytes": 123456 } ]` |
| `GET /api/log/2026-09.csv` | CSV download, PIN-protected (§12). The month is re-emitted in one explicit schema (v3, §9.1) whatever mix of row versions it holds; formula-leading text is quote-prefixed in the export only. One download at a time: a second concurrent one gets 503 `{"error":"a log download is already running, try again"}` |
| `POST /api/ota` | multipart `firmware` field; reboots on success. One at a time. No file → 400 `no firmware file`; too little heap or a fragmented one → 503 naming which check failed; an image built for a different board → 400 `firmware is for a different board (expected <board>)`; larger than the OTA slot → 413. Answers 200 only after the final chunk arrived *and* `Update.end()` succeeded |
| `POST /api/reboot`, `POST /api/wifi/reset` | Maintenance |
| `POST /api/pin` | Body `{"pin":"new"}`, authenticated with the **current** PIN in `X-Pin`. New PIN: 4–32 printable ASCII, no whitespace. → `{"ok":true}`, or 400 with the rule that was broken. No reset-by-network path: recovery is the serial console or the device info screen (§12) |
| `GET /api/debug/ui` | Test hook (not for the web UI; open, read-only): current page (main/night/stats/device), dimmed + applied brightness, due/chime counters, active profile, shown stops, hidden alternative panels, ticker text, header weather, LVGL pool use, resolution, heap |
| `POST /api/debug/tap` | Test hook (PIN-protected: it changes what the screen shows): simulated touch (press + click on the LVGL task), so page cycling and quiet-hours wake can be exercised without the panel |
| `POST /api/debug/oom` | Test hook (PIN-protected: it starves every other task for under a millisecond): exhausts the heap on purpose, forces a `std::bad_alloc`, frees everything and answers `{"caught":true,"blocks":N,"largest":B,"free_before":X,"free_after":Y}` - the deterministic proof of the emergency exception pool (§12.1). `largest` under ~100 means the exception object could only have come from the pool; without the pool the request reboots the device |

Arrival object in `/api/state`:
```json
{ "trip": "3667", "vehicle": "7477", "destination": "20th-Johnston",
  "predicted": 1789353562, "scheduled": 1789352700, "eta_s": 1262,
  "late_min": 13, "late_known": true, "status": "live", "seats": "FEW_SEATS_AVAILABLE",
  "sched_trip": "281757" }
```
`status` ∈ `live` (has RT prediction), `scheduled` (schedule only), `skipped` (RT says the stop
is skipped), `unknown`. `sched_trip` is the static-GTFS trip id (`BusSchedules` `trip_id`) this
row was matched to, `""` if none — it is what lets §9's tracker reconcile the scheduled and live
records for one trip. A `skipped` row may have no `predicted` at all: the feed often reports a
skip with no time, and it is then shown at its matched scheduled time rather than dropped.

Each `stops[]` entry carries its own health, because the sources differ per stop and one
failing must not condemn the others (`transit::StopSnapshot`, `lib/transit_core/model.h`):

| Field | Meaning |
|---|---|
| `ok` | every source **this stop** needs answered with usable, current data. A bus stop needs TripUpdates + TransitView + BusSchedules; a subway stop needs BusSchedules only (§4.6); a rail stop needs Arrivals. `last_poll.ok` is the AND of these. |
| `error` | short human reason when `!ok`: `"live feed truncated"`, `"SEPTA schedule unavailable"`, `"live feed stale"` |
| `health` | what the rows shown are worth: `live` (a fresh realtime prediction), `schedule_only` (scheduled times only — subway, or a live source that failed or had nothing), `stale` (the feed's own timestamp is more than 5 min old, or more than 5 min in the future; live rows are demoted to `scheduled` or dropped), `unavailable` (a needed source failed and there is nothing trustworthy to show) |
| `source_ts` | when SEPTA **produced** the data (GTFS-RT `FeedHeader.timestamp`, else the newest TransitView vehicle timestamp), as opposed to when we fetched it. `0` = the feed published none, so its age is unknown; freshness is never invented from the fetch time. |

## 8. Display UI (LVGL 9)

Main screen (portrait by default; every size derives from the runtime resolution so rotation just re-flows it):
- Header (10 % height): device name (off by default), clock (12 h), current weather (a 24 px colour condition icon from `src/icons/` and `69°`, sun/moon by local hour; SS4.8; was `69° mostly
  clear`), Wi-Fi bars, "updated 12 s ago" - each switchable via `device.header`. The Wi-Fi
  signal is four bars filled solid like a phone's status bar - no glyph, no number - at the
  height of the small font's line: RSSI ≥ −55 dBm lights 4, ≥ −65 3, ≥ −75 2, ≥ −85 1, weaker
  or disconnected 0, and the unlit bars stay faintly visible so "one bar" and "no signal" both
  still read as the icon (`ui_common.cpp` `makeWifiBars`/`setWifiBars`; on the amber stale
  header they take its dark-on-amber colours like every other header item).
- One panel per stop the active profile shows (§6 `profiles`; all stops otherwise), stacked; each
  has a title row per `title_style` (`17 Southbound → 20th-Johnston` by default; bullet, not middle
  dot, in the route form - the built-in font lacks U+00B7), an optional weather note about the hour
  of the next arrival (§4.8, only when notable), and `show` arrival rows (2 in large-text mode,
  minutes in the 48 px digits font from `src/fonts/`): route badge (route colour from config or default), destination
  (ellipsized), big minutes right-aligned (`12`, `Due` when < 1 min, `Now` when 0, the clock time
  `1:14a` from 60 min out so an overnight or wrong-day schedule row never reads "958"), and a badge:
  green `on time` (−1..+5 min, SEPTA's own on-time definition), red `+13`, blue `−2`, grey `sched`,
  orange `skip`. With `device.crowding`, SEPTA's seat estimate sits after the destination as a
  word (`open`, `few seats`, `standing`, `packed`, `full`), a three-slot chair/person meter, or
  both (SS6). While a bus is within `due.minutes` its minutes blink red
  (`due.screen`), the LED blinks green (`due.led`) and, once per live trip, the speaker beeps twice
  (`due.chime`, silenced in quiet hours).
- An alternative panel (`alt_of`) is hidden until its primary stop has nothing within
  `alt_after_min`; then it takes the primary's place below it.
- Indego section above the ticker when `bike` is on: a panel headed `[bicycle] Indego` (with
  `12 min old` in amber on the right once the feed is older than 10 minutes, or `no data yet`),
  then one row per station: the configured name, ellipsized, and the counts right-aligned per
  `bike.style` (SS6). A station missing from the feed reads `no data`; an inactive one `offline`.
  The glyphs come from the 16 px icon font in `src/fonts/` and are coloured with LVGL's inline
  recolor, one label per station.
- Night page (`night`): when every shown stop has nothing within `after_min`, the arrivals page is
  replaced by a big clock, the date, the header weather, and `17 Southbound: next 5:12a (sched)`
  per stop. Tapping cycles pages exactly as from the arrivals page. A stop whose `health` is
  `Stale` or `Unavailable` **keeps the arrivals page up**: "nothing is due" and "we cannot see what
  is due" are different answers and only the first earns the clock. `ScheduleOnly` does not block
  it — a subway stop only ever has schedule rows.
- Quiet hours (`quiet`): the backlight drops to `brightness` (0 = off) inside the window; any touch
  restores it for `wake_seconds` without changing page.
- Footer ticker when alerts exist and `ticker_show` is not `off`: `17: <alert>` and `17 detour:
  <detour>` for each distinct detour (either half alone with `alerts` / `detours`). Height is `device.ticker_lines` lines of the small font (default 3); one line scrolls
  sideways, more wrap and scroll upward credits-style, both at `device.ticker_speed` px/s via our
  own `lv_anim` (LVGL's built-in label scroll caps a pass at 10 s, unreadable for a paragraph).
  Static when the text fits.
- Light theme by default, dark via `device.theme`; every colour comes from `ui_common.cpp` so the
  screens rebuild in the new palette on a config change. `device.invert_colors` fixes panels that
  render inverted (§6).
- Stale data: header turns amber with dark text and "stale 4 min"; no data: panel shows the reason.
  Per stop, that same caption line also carries the stop's **health** (`StopSnapshot::health`,
  §4.7) whether or not there are rows under it: `schedule only` when the rows are timetable rather
  than tracking, `stale N min` measured from `source_ts` (when SEPTA produced the data, *not* when
  we fetched it — a feed re-fetched every 30 s is always "just fetched"), and the stop's own
  `error` text when it is `Unavailable`. Three schedule rows with no caption look exactly like
  three live ones. Rows are shown whenever the stop has rows, not only when its whole fetch
  succeeded: a truncated live feed with a good schedule is `ok=false` *and* has times worth
  showing. `Unavailable` is the one state that shows the reason and no rows. A `Skipped` row with
  no prediction renders from its scheduled time with the orange `skip` badge; a row with neither
  time shows `--`.
- Tap anywhere cycles Main → Stats → Device info → Main. The three pages are built from the same
  pieces (`ui_common.cpp`: the header strip, the padded column of panels, the stop-style panel,
  the route badge, the fonts and palette) so they match by construction rather than by copying
  numbers around. The stats and device pages are built when tapped to and freed when tapped away
  from: LVGL's 36 KB pool holds the arrivals page (~20 KB with two stops) plus the night page,
  and keeping all four resident left no room for a four-stop configuration.
- Stats page: header `Statistics  last 30 days` with `tap for device info` on the right (hidden
  at 240 wide); one stop-style panel per configured stop, titled with the route badge and the
  main page's title for that stop. Per stop, last 30 days (§9.2): the **on-time %** in the big
  minutes font, green from 80 %, amber from 60 %, red below (the web Stats page's thresholds),
  over an **on-time meter** - a full-width track with the on-time share filled from the left in
  the same colour, the glanceable form of the number; the **mean lateness** in the arrival badge
  colours (green inside SEPTA's −1..+5 min window, red late, blue early); the **worst hour**; the
  **ghost count**; and a footer `123 arrivals • 40 inferred`. Each panel picks one of three
  layouts from the height it will get (the area below the header shared equally, like the
  arrivals page), never from the board name: *Tiles* (two stops on a 320-tall board: title /
  `87% on time` with a 6 px meter under it / three captioned tiles / footer, spread top to
  bottom), *Compact* (two stops on a 240-tall board, four on a 320-tall one: the footer rides on
  the title row as `n=123 • 40 inferred` beside the stop's short label, a 3 px meter rules under
  the title, four two-line tiles along the bottom; captions shorten to `avg`/`worst` at 240
  wide), *Line* (four stops on a 240-tall board, ~49 px each: the title row and rule, then
  `87% on time • +1.3 min late • 3 ghosts` - the worst hour is the fact that does not fit).
  On-time % is a **dash** over an empty meter, never `0%`, when no arrival's lateness was ever
  known; a stop whose summary has not been computed yet reads `loading…` and one with no samples
  `no data yet`, with the numbers and the meter hidden - never zeroes. The labels are only
  rewritten when the cached summary actually changes (every ~10 min), not on every 1 Hz tick.
- Device info page: header `Device info  v0.2.0` with `tap for arrivals`. A *Network* panel: the
  Wi-Fi bars and SSID on its title row, the mDNS URL in link blue and the body font (the one line
  the owner has to be able to find), the IP with the RSSI in dBm on the right, and
  `SEPTA  ok, 12 s ago` or `SEPTA  failed 4 min ago: <error>` from the poller's last attempt
  (`getPollStatus()`; the arrivals header only ever says "stale"). A panel titled with the
  device name (`device.name`), `up 2d 5h` on its title row: the web PIN in the big minutes font
  with `web PIN` beside it (on a ≥ 320-tall board and up to 8 characters; a longer custom PIN
  takes the body font and wraps, never ellipsizes - a PIN the owner cannot read off the screen
  is a PIN they have lost), `SD  mounted, 3720 MB free` - or `N rows dropped: <error>` in red
  while writes are failing and amber once they land again (sd_logger.h, F26) - and
  `heap  75 KB free`. Both panels are content-height (a mostly-empty card looks like a fault);
  `reset Wi-Fi: hold 5 s` is a bordered button along the bottom edge, the only clickable child on
  any page.
- Sizes derive from `lv_display_get_horizontal_resolution()` so 320x240 gets 2 rows per stop
  and smaller fonts; 480x320 gets 3 rows. That is the per-panel **capacity**; each stop actually
  gets its own `show` (1..4, §6) clamped to it, so a stop asking for one row gets one.

## 9. Logging and statistics

### 9.1 Event log (SD, `/transit-log/YYYY-MM.csv`)
Header row on file creation. Columns (log schema v3, since 2026-09-15):
```
ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,late_min,horizon_s,headway_s,note,seats,temp_c,wx,alert,bikes,ebikes,docks
```
Three shapes exist and all three must keep parsing; nothing ever rewrites old rows:

| schema | since | shape |
|---|---|---|
| v1 | (initial) | 14 columns, through `note`. `seats` and the six trailing columns unknown. |
| v2 | 2026-09-14 | 21 columns. `temp` is whatever unit the device was displaying. |
| v3 | 2026-09-15 | the same 21 columns, `temp` renamed `temp_c` and **always Celsius**. |

**Temperature unit (v3, 2026-09-15).** The column used to hold the device-unit temperature with
nothing in the row saying which unit that was, so a log was only interpretable if you also knew
how the device had been configured — and the owner's unit was logging °F. From 2026-09-15 the
column is Celsius always; the app converts for display. v2 and v3 rows are the same shape and are
*not* distinguishable row by row: only the file's header (`temp` vs `temp_c`) and the cutover date
say which unit a given row is in. Rows before 2026-09-15 on the owner's device are °F.

`transit_stats::csvHeader()` is the **single source of truth** for the header line, and
`csvSchemaVersion()` returns 3. No other file may spell the column list out again (sd_logger.cpp
used to keep its own copy, which stayed at 14 columns while rows grew to 21).

**Bounded records.** A complete record never exceeds `kMaxCsvLineBytes` (512) and never contains a
CR or LF inside a field: `toCsv()` replaces control characters with a space and truncates free-text
fields (64 chars for `note`/`trip`/`vehicle`, 32 for `stop_key`/`route`/`dir`/`seats`). That is
what lets a reader work a line at a time with a fixed 512-byte buffer without tracking quote state.
Readers must skip — never abort on — a blank line, a header row, or a record that fails to parse:
one damaged record must cost exactly that record and nothing after it.

**Export.** `normalizeCsvLine()` rewrites a v1 or v2 row as a full 21-column row so a mixed
historical month downloads in one explicit schema, and prefixes any text field starting with
`= + - @` with a single quote so spreadsheets treat it as text. That prefixing happens **only in
the export**, never in the stored file — a log you cannot byte-compare against the device is not a
log. (A 21-column row's temperature is passed through unchanged; see the unit note above.)
The firmware side of that is `transit_app::LogExport` (`sd_logger.h`): it opens one monthly file,
emits `csvHeader()` once whatever the file's own first line says, and streams every stored line
through `normalizeCsvLine()` into an AsyncWebServer chunked response. The raw file stays available
verbatim; this is the "open it in a spreadsheet" path, not a replacement for it.

**A failed poll is not an observation** (the governing rule, 2026-09-15). While polling for a stop
is failing, `ArrivalTracker::observe()` records the outage and changes nothing else: no `arrive`,
`ghost`, `noshow` or headway can ever be produced by a network timeout, and tracked trips are
frozen rather than reaped. Inference resumes only from fresh, successful observations, and a
disappearance must be confirmed by **two consecutive successful polls**
(`kMissesBeforeInference`) before anything is concluded from it — one missing observation is
routinely a partial GTFS-RT feed (§4.2), not a bus. Callers must pass `poll_ok` honestly.

Events emitted by `ArrivalTracker` from the stream of `StopSnapshot`s:
- `pred` — a prediction snapshot for (stop, trip) at the first sighting and when the ETA first drops
  under 900, 600, 300, 120 s. `horizon_s` = predicted − ts at that moment. `seats` is the
  crowding token current at that sighting (see below).
- `arrive` — **inferred** passage, never a measured one: the stop's `stop_time_update` for that
  trip stopped being published on two consecutive successful polls. `actual_ts` = the last
  predicted time when the trip vanished within ±120 s of it, else the first missing observation's
  timestamp. `note` records which inference was used — `inferred` (vanished near its prediction),
  `late-vanish` (still being predicted well after it should have arrived), `unobserved` (closed
  after an outage; see below) — and `horizon_s` carries the horizon that was left at the moment we
  lost sight of it. Include `late_min` from TransitView at that moment and `scheduled_ts` = nearest
  BusSchedules entry to (`actual_ts` − late) within 10 min, if any. `headway_s` = gap since the
  previous `arrive` for the same stop and direction — see the continuity rules in §9.2. `seats` is
  the last crowding token seen for that trip before it vanished.
  A live trip retires the pending scheduled record it corresponds to (matched by trip id, or by
  equal scheduled time, since SEPTA's static and realtime trip ids differ — §4.4), so one bus can
  never produce both an `arrive` and a `noshow`.
- `ghost` — a tracked trip vanished (again, confirmed over two successful polls) while its
  prediction was still > 180 s in the future and it never arrived.
- `noshow` — a scheduled departure passed by more than 10 min without any live trip matched to it,
  while the route had at least one live vehicle (otherwise it is an `outage`). A pending scheduled
  record whose deadline passed *during an outage* is dropped silently instead: we were not
  watching, so "the bus never came" is not something we know.
- `outage` — polling failed for more than 5 min; one line at start and one at end (`note="end"`).
  **The start row's `ts` is the first failed poll**, not the moment the 5-minute threshold was
  crossed (otherwise every outage is short by the detection delay — a 600 s outage recorded 299 s);
  the detection delay itself is kept in `horizon_s`. This is the one kind of row whose `ts` can be
  earlier than the row before it in the file. On recovery, a trip whose predicted time passed while
  the device was blind is closed as an `arrive` with `note="unobserved"` and no `headway_s` — an
  explicit "a bus probably passed and we did not see it", never a fabricated ordinary arrival, and
  never counted twice if the trip is still being predicted when polling comes back.
- `bike` — one row per configured Indego station per hour (`bike.stations`, §6), built by the app
  layer, not `ArrivalTracker`. `stop_key` = `indego-<station id>` (e.g. `indego-3468`); route/dir/
  trip/vehicle are empty; `note` = the station's display name; `bikes`/`ebikes`/`docks` carry that
  hour's Bicycle Transit status feed counts (§4.9). A row is written only from a sample actually
  observed within the last 15 minutes, and never twice from the same fetch: the display
  deliberately keeps showing the last good counts when the 400 KB feed fails, but writing those
  into the log as *this* hour's observation would be inventing data. An unknown e-bike count stays
  empty; it used to be written as `0`, which reads back as "this station has no e-bikes".

New columns (all optional; empty means unknown, never coerced to 0):
- `seats` — crowding token, one of `empty`, `open`, `few`, `standing`, `packed`, `full`; set on
  `pred`/`arrive` rows only, from SEPTA's `estimated_seat_availability`
  (EMPTY→`empty`, MANY_SEATS_AVAILABLE→`open`, FEW_SEATS_AVAILABLE→`few`,
  STANDING_ROOM_ONLY→`standing`, CRUSHED_STANDING_ROOM_ONLY→`packed`, FULL→`full`).
- `temp_c`, `wx` — temperature in **Celsius** (schema v3, see above) and WMO weather code at `ts`
  (§4.8), filled in by the app layer on `pred`/`arrive`/`ghost`/`noshow` rows.
- `alert` — 0 none, 1 a service alert was active for the route, 2 a detour was active; filled in by
  the app layer on `pred`/`arrive`/`ghost`/`noshow` rows.
- `bikes`, `ebikes`, `docks` — `bike` rows only (see above).
Rotation: a new file each month; refuse to log when SD free space < 50 MB and show a warning.

**A write that did not land is not a non-event** (2026-09-15). `File::println()` returns the bytes
it actually committed, and a card that has been pulled or has filled up returns a short count
rather than failing, so a writer that ignores the return value writes half a row and reports
success. Every append checks both the header and the row, counts a failure in `sd.dropped_rows`,
clears `sd.write_ok` and records a short `sd.error` — all three on `GET /api/state` — and logs a
line on the serial console. A card that has gone away is marked **unmounted** with "card removed"
rather than continuing to report free space and healthy logging.

**Temperature is logged in Celsius by the app layer**, converting from whatever unit the display
is configured for, and is left empty when the forecast behind it is stale (over an hour old): an
unknown value is empty, never a stale one dressed as current.

### 9.2 Stats definitions
- On-time: SEPTA's definition, no more than 59 s early and no more than 5 min 59 s late.
  **The denominator is arrivals with a KNOWN `late_min`, not all arrivals** (`late_known`). An
  arrival whose lateness we never learned is not evidence that the bus was late; counting it as a
  miss made one on-time bus plus one unknown read as "50% on time". With `late_known` = 0 the
  answer is *no data* — JSON `null`, and `StopSummary::has_on_time` false — never `0`.
- Lateness by hour (24 bins) and weekday (7 bins): count, mean, and a −10..+60 min histogram per
  bin (approximate median from the histogram).
- Headway: actual `headway_s` vs scheduled gap; bunching = actual < 40 % of scheduled;
  gap = actual > 175 % of scheduled. Report counts and a histogram of the ratio.
  Only **positive** gaps of at most 3 h count: a negative or zero `headway_s` is corrupt or
  ambiguous (the aggregator ignores it defensively), and a longer one is a service break rather
  than a wait. The tracker never writes a headway at all across a poll outage, a local
  service-day boundary, a gap over 4 h, an `unobserved` closure, or a stop re-registered onto a
  different route/direction: a headway claims two buses were consecutive, and if we cannot see
  that they were, we do not make the claim. When several trips are inferred to have passed in one
  observation they are sorted by inferred passage time before headways are computed (slot order
  used to produce negative gaps); two passages inferred at the same instant get **no** headway
  rather than a 0.
- "Typical wait" is **half the mean observed gap** (`wait_basis: "half_mean_gap"` in the response)
  — the random-arrival expectation *if* buses were evenly spaced, over the gaps that survive the
  filters above. It is not a measured wait, and the UI must not present it as one.
- Ghost and noshow counts per route/direction/day.
- **Forecast stability** (renamed from "prediction accuracy", 2026-09-15): for each `arrive`,
  revision = predicted (at horizon bucket 120/300/600/900) − the final inferred arrival time,
  reported as `mean_abs_revision_s` and `mean_revision_s` per bucket. This is *not* accuracy:
  `actual_ts` is itself derived from the last prediction, so the forecast is being compared with
  itself and a feed that is confidently wrong and never revises scores perfectly. What it honestly
  measures is **how much the forecast moved between first sighting and the final inferred
  arrival**. Never label it error, and never present an `arrive` row's `actual_ts` as a measured
  passage time — every arrival in this log is an inference, and `inferred` counts the ones whose
  `note` says which inference it was.
- Coverage: the fraction of the requested window with successful polling, derived from `outage`
  rows by overlapping each outage interval with the window (including an interval that started
  before the window and one still open at its end). Outages shorter than 5 minutes are not logged,
  so coverage is an upper bound on real coverage, never a lower one.

### 9.3 `/api/stats` response
```json
{ "stop": "17-21332", "days": 30, "samples": 812, "late_known": 763,
  "on_time_pct": 71.4, "mean_late_min": 3.2, "inferred": 812, "unobserved": 6, "coverage": 0.9832,
  "by_hour": [ { "h": 8, "n": 41, "mean": 5.1, "p50": 4, "p90": 12 } ],
  "by_weekday": [ ... ],
  "headway": { "n": 700, "bunched": 61, "gapped": 44, "ratio_hist": [ ... ] },
  "ghost": 9, "noshow": 4, "outage_min": 37,
  "forecast_stability": [ { "horizon_s": 300, "n": 500, "mean_abs_revision_s": 74,
                            "mean_revision_s": 21 } ],
  "crowding": { "by_hour": [ {"h":0,"n":12,"mean":1.4,"dist":[0,7,3,2,0,0]} ],
                "by_weekday": [ {"wd":0,"n":0,"mean":0,"dist":[0,0,0,0,0,0]} ] },
  "wait_by_hour": [ {"h":8,"n":14,"mean_gap_s":540,"max_gap_s":1320,"ghost":1,"noshow":0} ],
  "wait_basis": "half_mean_gap" }
```
`on_time_pct` and `mean_late_min` are `null` (not `0`) whenever `late_known` is 0, and every
response carries `samples`, `late_known`, `inferred`, `unobserved` and `coverage` so the UI can
always distinguish "nothing happened" from "we could not see" (§9.2). `outage_min` is the overlap
of every outage interval with the requested window, including one still running.
`by_hour`/`by_weekday` always carry 24/7 entries, in hour/weekday order, even for hours with no
samples. `crowding` is built from `arrive` rows with a known `seats` (log schema v2, §9.1):
`dist[6]` is a count per crowding level in the order empty/open/few/standing/packed/full, and
`mean` is the mean level index (0 = empty .. 5 = full), one decimal, 0 when `n` is 0. `wait_by_hour`
is a 24-entry, always-present per-local-hour breakdown of the actual headway: `n`/`mean_gap_s`/
`max_gap_s` come from `arrive` rows with a known, positive `headway_s`; `ghost`/`noshow` are the
counts of those event rows falling in that hour. All computed in one streaming pass with
fixed-size accumulators (< 9 KB).

`GET /api/stats/overview?days=N` streams every monthly log file in the window ONCE for every
configured stop and Indego station (`OverviewAggregator`), rather than one pass per stop like
`/api/stats` above — a cheap landing-page summary, not the detailed per-stop breakdown:
```json
{ "days": 7, "samples": 812, "inferred": 812,
  "stops": [ { "stop": "17-21332", "samples": 812, "late_known": 763, "on_time_pct": 71.4,
               "mean_late_min": 3.2, "inferred": 812, "ghost": 9, "noshow": 4,
               "outage_min": 37, "coverage": 0.9832, "last_seen_ts": 1757900000 } ],
  "bikes": [ { "station": "indego-3468", "name": "Snyder & Dorrance", "samples": 168,
               "by_hour": [ {"h":0,"n":7,"bikes":4.3,"ebikes":2.1,"docks":8.6} ] } ],
  "excluded_stops": 2, "excluded_bikes": 0 }
```
The caller passes the **currently configured** stop keys and Indego station keys to
`OverviewAggregator`, and those get reserved slots, in that order, ahead of anything else in the
window — a reserved key appears even with zero rows ("just added, no data yet" is a real answer).
Remaining slots go to other keys in first-seen order, up to config's own caps (8 stops, 3 bike
stations, §6). Keys beyond that are counted in `excluded_stops`/`excluded_bikes`, which the UI
must disclose rather than present a partial list as the whole truth: without the reservation, a
month in which the user changed stops filled all eight slots with the stops they *used* to watch
and silently dropped the ones on the screen. `samples`/`inferred` at the top level are totals over
the stops actually listed. `on_time_pct`/`mean_late_min` use the same definitions (and the same
`null` rule) as §9.2; `last_seen_ts` is the max `ts` of any row (any event type) for that stop.
Each bike station's `by_hour` is a 24-entry, always-present per-local-hour breakdown of that
hour's `bike` rows; `name` is the display name from the most recently seen row. Fixed-size
accumulators, aiming under 2 KB.

## 10. Web UI (`web/`)

Single page, vanilla JS, no framework, no build step other than `web/build.mjs`, which strips
whole-line comments from `app.js`/`app.css` and gzips `index.html`, `app.js`, `app.css`,
`favicon.svg` into `firmware/src/generated/web_assets.h`. The stripping is a packaging step only —
`web/` on disk and the mock server keep every comment — and it is deliberately conservative: only
lines that are *entirely* a comment go, the source is refused if it ever grows a multi-line template
literal, and the stripped JS is parse-checked before it is embedded. It buys back ~4.6 KB gzipped,
which is what lets the files stay commented at the density the rest of the repo uses.
`web/mock-server.mjs` implements §7 against the fixtures so the UI can be developed with
`node web/mock-server.mjs` and no hardware; its admin PIN is `123456`.

Views: **Now** (live arrivals as the device sees them, with a per-stop feed-health chip from
`health`/`source_age_s`), **Stops** (list, reorder, add/edit/remove;
add flow: mode → route + a bus/trolley choice → stop list from `/api/proxy/stops` with a Leaflet map
loaded from a CDN when online → direction learned from `/api/proxy/schedule` → label; Regional Rail
picks its line from a dropdown of SEPTA line codes), **Stats** (per stop, charts in
inline SVG: by-hour bars, weekday bars, headway ratio histogram, forecast-stability histogram,
ghost/noshow tiles; CSV download), **Settings** (device, brightness, poll, logging, OTA upload,
Wi-Fi reset, firmware version, board, Web PIN). Total gzipped assets must stay under 60 KB.

Every control on Settings, and the Indego card and stop edit form on Stops, carries a
`<p class="hint">` in plain language under it: the owner is not a programmer, and an unexplained
setting is one nobody dares change.

### 10.1 Admin PIN in the UI

The calls that change the device — `PUT /api/config`, `POST /api/reboot`, `POST /api/wifi/reset`,
`POST /api/ota`, `POST /api/pin` — and `GET /api/log/<file>.csv` carry an `X-Pin` header. The PIN
lives in `localStorage` under `ptd_pin`. `fetchJSON(url, opts, true)` adds the header; a `401`
("pin required" / "wrong pin") opens a modal — *"This device asks for its PIN before changing
settings. Find it on the device: tap the top of the screen to open the device info page, or read it
from the serial console."* — stores what is typed and replays the request **once**. A second `401`
is reported as "That PIN was not accepted" rather than echoing the firmware's two words. A `429`
(`{"error":"too many attempts","retry_s":N}`) reports the wait and is **not** retried.
`uploadFirmware` carries the header on its `XMLHttpRequest` and words `409` as "already installing
an update" and `400` with the firmware's own message (wrong board, bad image). The CSV links are
buttons, not `<a href>`: a plain link cannot carry a header, so the bytes are fetched and handed
over as a Blob + object URL + synthetic click.

Settings gains a **Web PIN** card: change the PIN (new + confirm, 4–32 printable ASCII with no
spaces, the current PIN supplied by the same header) and "Forget PIN on this browser", which clears
only this browser's copy and says so. `/api/state`'s `auth.pin_required`, `board` and
`config_recovered` are all surfaced there — the last as *"The device restored its previous settings
after a bad save."*

### 10.2 Honest numbers on the Stats page

Two rules, because a statistic that overstates its own certainty is worse than no statistic:

1. **A missing value is never drawn as 0.** `on_time_pct` can be `null` (SEPTA never said how late
   anything ran); it renders as "no data", not a red 0 %.
2. **A percentage always shows the count behind it** — "On time: 71 % of 143 with lateness data
   (190 arrivals)".

The page also states what the device can and cannot know. Arrivals are **inferred** from a bus
disappearing from the live feed, not measured, and `unobserved` counts the trips that fell in a
stretch when the device was not polling at all — distinct from ghosts and no-shows, which are
failures it actually watched happen. What used to be "prediction accuracy by horizon" is now
**forecast stability** (`forecast_stability[]`, the renamed `prediction[]`): how far the forecast was
*revised* between first sighting and the inferred arrival, bar = `mean_abs_revision_s`, dot =
`mean_revision_s`. `wait_basis` names the estimator ("half the average gap between buses") instead of
the page asserting a method the firmware may have changed, `coverage` (0..1) is shown as "data
coverage 97 %" per stop and for the window, and the overview's `excluded_stops`/`excluded_bikes` are
reported as "N older stops not shown" so a missing row does not read as missing data. Keys are read
defensively (`samples ?? arrivals`, `late_known ?? late_known_n`, `mean_abs_revision_s ?? mae_s`) so
a rename on the firmware side degrades to a label rather than a blank page.

On the Now page, `ok` is false for a **stale** stop as well as an unreachable one, but a stale stop
still has usable — if old — times: it keeps its arrival rows with an amber "stale N min" chip and the
short `error` text above them, and only a stop that is genuinely empty or unavailable gets the red
banner in place of its rows.

### 10.3 Untrusted text

Agency and user text reaches the DOM only through the `h()` helper, which appends text nodes; the UI
contains no `innerHTML`, `insertAdjacentHTML` or `outerHTML`. The one third-party sink that renders
HTML is Leaflet's `bindTooltip`, which is therefore handed an element, never a string. Leaflet's CSS
and JS are pinned to 1.9.4 and loaded with Subresource Integrity plus `crossorigin="anonymous"`; a
hash mismatch simply drops the map and the wizard falls back to its list-only view.

## 11. Portability

`transit_core` exposes a `TransitSource` interface (`fetchRealtime`, `fetchSchedule`,
`fetchAlerts`) with `SeptaSource` as the only v1 implementation. The GTFS-RT decoder is agency
agnostic. Adding an agency that has GTFS-RT TripUpdates plus any per-stop schedule endpoint
should be a new `*_source.cpp` and a config `agency` field, nothing else.

## 12. Security posture

**Reading is open, changing is not.** The device answers anyone on the LAN who asks what the
arrivals are; it does not let them change the configuration, reboot it, wipe its Wi-Fi, flash it,
or download the movement log. Firmware never contacts anything except SEPTA, Open-Meteo, Indego
and NTP. No telemetry. Wi-Fi credentials live only in the ESP32 NVS.

**Admin PIN.** A per-device six-digit PIN, generated with `esp_random()` on first boot and stored
in NVS (namespace `ptd`, key `pin`) — *not* in `/config.json`, because `GET /api/config` is open.
It goes in an `X-Pin` header on the protected routes listed in §7. Wrong or missing → 401; five
consecutive wrong PINs lock every protected route for 30 s (429 with `retry_s`); the comparison is
constant-time. `POST /api/pin` changes it (4–32 printable ASCII, no whitespace) and needs the
current PIN. The owner learns the PIN from the serial console at boot (`[auth] web PIN: 123456`)
or the device info screen — both require physical possession of the display, which is the
deliberate recovery path: there is no way to reset it over the network.

**Cross-site and rebinding.** The `Host` header is checked on every request before any handler
runs and must name the device (its IP, `<device name>`, `<device name>.local`, `192.168.4.1` or
`localhost`, optional port, case-insensitive); anything else is 421. That is what stops a page on
the public internet from resolving its own domain to the device's LAN address and then talking to
it with the attacker's origin. CORS is not enabled, so a cross-origin page cannot attach `X-Pin`
without a preflight this server does not answer — a blind form POST therefore cannot carry the
PIN. `GET /` sends `X-Frame-Options: DENY`, `Content-Security-Policy: frame-ancestors 'none'` and
`X-Content-Type-Options: nosniff`.

**First-time setup Wi-Fi.** The setup AP is WPA2 with a per-device ten-character password
(NVS `ptd`/`ap_pass`, from an alphabet with no `0/O/1/l/I`), shown on the panel next to a QR code
of the standard `WIFI:` join URI so nobody has to type it. An open setup AP would let anyone in
range watch the owner's *home* Wi-Fi password being typed into the portal. A device that already
has credentials never opens that AP by itself: it retries its stored network indefinitely (5 s →
60 s backoff) showing "Connecting to …" and "Tap the screen to open Wi-Fi setup instead", so a
router reboot cannot put a provisioning endpoint on the air. Once open, the portal closes itself
(reboot) after ten minutes with no client joined.

**OTA.** Still unsigned — signature verification is not in this version — but no longer
unconditional: the upload needs the PIN (checked at the first chunk, before a byte is written, and
the connection is closed rather than draining 1.7 MB from an unauthorised caller), only one
upload runs at a time, a disconnect mid-image aborts the update, the image must fit the OTA slot,
and the stream must contain this board's `PTD-BOARD:<env>;` marker or it is rejected with a 400.
That last check is what stops a 2.4"-capacitive image from bricking a 3.5" resistive panel.

**Residual risks, accepted for now.** HTTPS is deferred (§2 and `firmware/README.md`: a TLS
session needs ~40 KB of heap with two 16 KB contiguous buffers this board cannot spare, and
dropping mbedTLS freed ~100 KB of flash). So **the PIN travels over the LAN in clear text** — 
anyone who can passively sniff the owner's own network, or who already controls a device on it,
can read it and then do anything the owner can. The PIN raises the bar from "any script that finds
the device" to "an attacker already inside the network with packet capture"; it is not a defence
against that second attacker. Firmware is unsigned, so anyone who *has* the PIN can flash
arbitrary code. SEPTA and the other feeds are fetched over plain HTTP, so their contents are
spoofable by that same on-path attacker (arrival times are not a secret, and nothing in the
response is executed).

### 12.1 Memory posture (2026-09-14)
The classic ESP32 has ~320 KB of DRAM and no PSRAM; with LVGL, Wi-Fi, the async web server and the
feature set of §6 there is ~75-80 KB of heap free at runtime. Big long-lived objects are allocated
before Wi-Fi, every large allocation is `nothrow`, proxied bodies stream through LittleFS, and
`/api/state` answers 503 instead of a truncated document when it cannot be built. Known limit: four
simultaneous `/api/state` requests can leave the last one as an empty HTTP 200 (the network stack's
send buffer, not the handler); the device stays up. Scripted clients should treat an empty 200 as a
retry. See `firmware/README.md` "Memory and flash budget" for the numbers and the knobs.

Addendum (2026-09-15): C++ exceptions are enabled in this SDK (`-fexceptions`), so a `std::vector`
growth or `reserve()` that cannot get memory throws `std::bad_alloc`, and an uncaught throw is
`std::terminate` = reboot. The largest free block between polls on the owner's board is only
10-32 KB, so a cap-sized `reserve()` (a 64-update or 48-vehicle block is ~10 KB contiguous) turned
into a boot loop. Rules: retention caps are small (32 feed updates, 32 vehicles) and vectors grow
from a few entries instead of reserving the cap; `pollOnce()`, queued proxy/stats jobs and
`PUT /api/config` catch `std::bad_alloc` and report "out of memory" (a failed poll with per-stop
errors, or a 503) rather than resetting. Measured after the fix: heap ~54 KB minimum during a
poll, largest block ~32 KB median. **Invariant:** every long-running task that does STL allocation catches
`std::bad_alloc` at its top level, because an uncaught throw is `std::terminate` = reboot. There
are three: the poller task (net_poller.cpp, inner per-stop + outer cycle), the AsyncTCP web
handlers (web_server.cpp `guarded()` + the JSON-body handlers, answering 503) and its chunked-response
fillers (the log-export filler catches internally and truncates), and the LVGL display loop
(main.cpp `loop()`, skipping the frame). Any new task, handler or filler on either core must do the same. There was a hard case the wrappers could not catch
until 2026-09-16: when the heap is so exhausted that `std::bad_alloc` cannot allocate its own
exception object, `__cxa_allocate_exception` falls back to libstdc++'s emergency pool and, finding
none, calls `std::terminate` directly - and this SDK is built with a zero-byte pool
(`CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE=0`). The firmware now supplies the pool itself, without
rebuilding the SDK: libstdc++ sizes it at static-init by calling the weak hook
`__cxx_eh_arena_size_get()`, and `firmware/src/app/cxx_exception_pool.cpp` defines that hook (2 KB =
16 in-flight `std::bad_alloc`s at 128 B each, twice what the four allocating tasks can have
mid-throw at once) together with `__cxx_init_dummy`, so the SDK's `-u __cxx_init_dummy` is satisfied
by our object and `libcxx.a(cxx_init.cpp.obj)`, which carries the SDK's zero-returning definition, is
never linked. The file explains why `--wrap` cannot do this and why `--allow-multiple-definition` was
not used; the link map is the proof. Cost: one 2 KB `malloc` before `app_main()`, never freed; the
boot log prints it as `[heap] eh_pool arena=2048`. `POST /api/debug/oom` (PIN-gated, §7) is the
deterministic proof: it takes the heap away in shrinking blocks until even a 16-byte allocation
fails, forces a `std::bad_alloc`, frees everything and answers `caught:true`; without the pool that
request reboots the device. The device suite runs it. The heavy read handlers (`/api/state`,
`/api/config`) still refuse up front with a fixed-literal 503 when free heap is under
`kMinHeavyResponseHeap` (24 KB) or the largest block under 8 KB - no longer because the failure
would be uncatchable, but because a build that is going to fail costs CPU and heap the poller wants,
and a 503 the client retries is the cheaper answer. The same handlers are zero-copy since the same
date (`sendJsonStreamed()`): the finished document is moved into a holder a chunked response owns
and serialised straight into each TCP send chunk as the socket drains (the filler re-walks it per
chunk through ESPAsyncWebServer's `ChunkPrint`), instead of into a `String` that
`AsyncBasicResponse` then copied a second time. Peak heap per request drops from document + 2 x
body (plus `String` growth slack, and a body-sized contiguous block needed twice) to document + one
2 x MSS (2.9 KB) send buffer: roughly 6-13 KB less for a 5-8 KB `/api/state`, a wash for the
1.5-4 KB `/api/config`, and in both cases no body-sized contiguous block any more. Chunked transfer
encoding (no `Content-Length`) is the price; the small responses keep `sendJson()`'s `String`,
because for them the 2.9 KB send buffer would be the bigger allocation. ESPAsyncWebServer's own
`AsyncJsonResponse` was tried first and rejected for flash: it brings two more ArduinoJson
serializer instantiations (its typed fill and `measureJson`'s counting pass, +4.5 KB in all); the
streamed filler serialises through a `Print&` and `saveConfig()`'s two sinks now do too, so the
three share one instantiation and the streamed path costs about 0.8 KB of flash instead of 4.5.
What remains uncatchable, by design: C code that gets NULL from `malloc` and does not check it (no
throw, so no pool helps), a catch block that itself allocates with nothing left (building the 503
response object; it rethrows out of the handler), and the pool being finite. The convergence that
used to reboot the device - an invalid-stop configuration whose failed fetches depress the heap,
rapid repeated configuration saves (each parses a 16 KB body and rebuilds the screen), and
simultaneous `/api/state` reads - now ends in caught `bad_alloc`s (503s, a failed poll, a skipped
frame) and, if the heap stays wedged, the poller's self-heal reboot below; configuration is durably
saved either way. None of it occurs under normal use (single spaced reads, valid stops, occasional
saves), where the heap sits near 74 KB.

Task watchdog (2026-09-15): `CONFIG_ESP_TASK_WDT_PANIC=y` in this SDK, and HTTPClient waits for the
response line and each header in `Stream::timedRead()`, a busy loop that yields only to
same-or-higher-priority tasks, so at the poller's priority 1 it does not let IDLE0 (the task the
watchdog checks on core 0) run. A single read blocked for the whole 15 s fetch timeout therefore
panicked the watchdog. The fix is to bound one stream read/connect to `kStreamReadTimeoutMs` (4 s,
under the 5 s watchdog) in `http_fetch.cpp`; a streaming body still flows because each read returns
as soon as bytes arrive, and the overall fetch budget is kept by the absolute deadline and the
retry loop. (Dropping the poller to priority 0 also cured the watchdog but made it the lowest task
on core 0, so under web load it was starved while holding the snapshot mutex and the display task's
1 s `getSnapshot()` wait asserted in `vTaskPriorityDisinheritAfterTimeout`; priority 1 + the capped
read is the fix that avoids both.) Idle-slice work
(stats summaries, queued proxy jobs) additionally waits for 40 KB free heap and a 12 KB largest
block so it never collides with a config save on the web task.

Heap-wedge self-heal (2026-09-15): a long burst of rapid config saves (each rebuilds the whole
LVGL screen) interleaved with active polling can fragment the heap to ~2 KB largest block while
~50 KB is still free - too small for any fetch buffer, so every poll fails with a caught
`bad_alloc` and the board shows nothing new without crashing. Nothing defragments a running heap,
so the poller reboots itself after 15 consecutive failed polls while the largest block is under
6 KB. The largest-block guard is what makes this safe: an ordinary SEPTA outage leaves the heap
healthy, so it keeps its normal backoff and never reboots; only a genuine wedge, which a person
would fix by power-cycling anyway, triggers the reboot. Config is durably saved (this §), so the
reboot loses nothing. Observed cause was the on-device regression suite's stress section; normal
use holds the heap stable (~74 KB free, ~22 KB largest).

## 13. Milestones

- **M0** Repo, design, skeleton, CI that builds every env and runs host tests.
- **M1** Probe the owner's board; hello-world UI on hardware; confirm variant.
- **M2** Wi-Fi onboarding, config, web UI, live arrivals for route 17 on screen. Usable daily.
- **M3** SD logging, ArrivalTracker, stats in web UI and on screen.
- **M4** Alerts ticker, Regional Rail, subway schedule-only, web flasher on GitHub Pages, v0.1 release.

## 14. Work partition for coding agents

Agents work in separate directories and do not run `git` (the coordinator commits):
- **firmware skeleton**: `firmware/platformio.ini`, `firmware/boards/`, `firmware/src/`, `docs/hardware.md`.
- **transit_core**: `firmware/lib/transit_core/`, `firmware/test/test_core/`, may refresh `firmware/test/fixtures/`.
- **transit_stats**: `firmware/lib/transit_stats/`, `firmware/test/test_stats/`.
- **web**: `web/`, `firmware/src/generated/web_assets.h`.

Conventions: C++17, no exceptions, no RTTI, `-Wall -Wextra`, headers under `include/<lib>/`,
Unity tests under `test/test_<lib>/`, every parser tested against the real fixtures, every public
function documented in its header. Keep ArduinoJson as the only JSON library. Prefer `std::string`
/`std::vector` at this scale; avoid `String` in the libraries (fine in Arduino glue).
