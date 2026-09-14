# Philly Transit Display

A desk display for Philadelphia transit riders, running on the ESP32 "Cheap Yellow Display"
(CYD) family of boards. It shows the next buses, trolleys, or Regional Rail trains at the
stops you care about, with live lateness, and logs every arrival to the SD card so it can
tell you how the route actually performs over time.

- Design and decisions: [DESIGN.md](DESIGN.md)
- Hardware, pin maps, flashing: [docs/hardware.md](docs/hardware.md)
- Firmware build notes and flash budget: [firmware/README.md](firmware/README.md)
- Web UI and mock server: [web/README.md](web/README.md)
- Research that the design rests on: [docs/research/](docs/research/)

## Status

Alpha. As of 2026-09-14:

| Piece | State |
|---|---|
| Boots, display, touch probe, Wi-Fi setup hotspot | Verified on an ESP32-3248S035R (3.5" resistive) |
| Live SEPTA arrivals, lateness, alerts on screen | Verified on the same board against live SEPTA data |
| Light/dark theme, panel colour inversion, alert ticker height and speed, rotation | Settings in the web UI; apply without a reboot |
| Web UI: stops wizard with map, stats charts, settings, OTA | Verified on the device (OTA upload not yet exercised) |
| SD logging and on-device statistics | Logging and `/api/stats` verified on the device; 70 host tests pass; statistics need a few days of data to mean much |
| Other CYD variants (2.8", 2.4", capacitive) | Build in CI; not yet tested on hardware |
| Browser flasher on GitHub Pages | Published with each release at https://holdthedoorhoid.github.io/philly-transit-display/ |

## What it does

- Pick any SEPTA bus/trolley route and stop, subway station (schedule only), or Regional Rail
  station from a web page served by the device itself. No app, no cloud account, no API key.
- Show the next 2-3 arrivals per stop and direction with minutes-until and a late/early badge.
- Show SEPTA service alerts and detours for your routes.
- Show the current weather in the header and, when the forecast at a stop's next arrival calls for
  an umbrella, say so on that stop's panel. Weather data by [Open-Meteo.com](https://open-meteo.com/)
  (CC BY 4.0), no account needed.
- Tell you when to leave: when a bus is a few minutes out the row blinks, the LED blinks, and the
  board can beep twice. Show how crowded each bus is (SEPTA's seat estimate) as a word or a
  three-slot chair/person meter.
- Fit your day: commute profiles pick which stops show on weekday mornings vs evenings; an
  alternative stop appears only when your usual one is running late; quiet hours dim the screen
  overnight; a night clock with the first departures replaces the empty panels when nothing is
  due; large-text mode for reading across a room; per-stop titles.
- Indego bike share: classic bikes, e-bikes and free docks at up to three stations near your
  stops, from Bicycle Transit Systems' public status feed, drawn as an icon meter or in words;
  a count turns amber at one or two and red at zero.
- Log predictions and inferred arrivals to the SD card; view lateness by hour and weekday,
  headway bunching, ghost buses, and prediction accuracy in the web UI or on the screen.
- Work on the 2.4", 2.8", and 3.5" CYD variants from one codebase.
- Stay portable: the real-time decoder speaks standard GTFS-Realtime, so other agencies can be
  added without touching the display or stats code.

## Quick start

1. **Flash.** Either use the browser flasher at
   https://holdthedoorhoid.github.io/philly-transit-display/ (Chrome or Edge, board plugged in over
   USB, pick your board variant) or build it yourself:
   ```sh
   pip install platformio
   cd firmware
   pio pkg install -e cyd-3248S035R      # pick your board's env, see platformio.ini
   pio run -e cyd-3248S035R -t upload --upload-port /dev/ttyUSB0
   ```
   On Linux your user needs to be in the `dialout` group.
2. **Join Wi-Fi.** The screen shows a hotspot name like `TransitDisplay-29BC`. Join it from a
   phone or laptop; the setup page opens (or browse to `http://192.168.4.1`). Pick your network.
3. **Pick stops.** Open `http://transit-display.local`, go to Stops, and use Add stop: choose a
   route, pick the stop on the map or list, choose the direction. Save. The screen updates on the
   next poll.
4. **Stats.** Insert a FAT32 microSD card. After a day or so, the Stats page shows how the route
   really behaves at your stop; the raw CSV is downloadable from the same page.

The default configuration shows Route 17 at 19th & Mifflin in both directions.

## Development

```sh
cd firmware && pio test -e native          # transit_core + transit_stats host tests
node web/mock-server.mjs 8080              # web UI against captured SEPTA responses
node web/build.mjs                         # regenerate firmware/src/generated/web_assets.h
python3 tools/gtfsrt_decode.py             # inspect a captured GTFS-RT feed
```

Layout: `firmware/` (PlatformIO project; `lib/transit_core` parses SEPTA and GTFS-RT,
`lib/transit_stats` tracks arrivals and aggregates statistics, `src/app` is the Arduino glue),
`web/` (the device's web UI), `flasher/` (ESP Web Tools page), `docs/`.

## Data sources

SEPTA's public API and GTFS-Realtime feeds, no key required. See DESIGN.md section 4 for the
endpoints, the trip-id join that makes per-stop lateness possible, and the quirks discovered
along the way (letter route ids, intermittent 400/501 responses, subway route ids B1/L1).

## License

MIT. See [LICENSE](LICENSE).
