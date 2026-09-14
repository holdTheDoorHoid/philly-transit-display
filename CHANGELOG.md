# Changelog

## Unreleased

- Logging: every arrival row now records SEPTA's crowding estimate, the weather at the event
  (temperature and WMO code) and whether an alert or detour applied to the route; one `bike`
  row per Indego station per hour records bikes, e-bikes and free docks. Old 14-column rows
  still parse (DESIGN.md 9.1).
- Stats: `/api/stats` gains crowding by hour and weekday and an expected-wait / reliability
  by-hour table; new `/api/stats/overview` compares every stop (and charts Indego availability
  by hour) in one pass. The web Stats page shows all of it, with an all-stops table on top.
- Web: the Indego card on the Now page is styled like the stop panels.
- Fixed a boot-time crash: Arduino's first hostname lookup clears lwIP's DNS cache from the
  caller's task, and if SNTP's own lookup was in flight the SNTP callback asserted without the
  core lock. The firmware now resolves the NTP host once before starting SNTP.
- Header weather is a colour icon (sun, moon, cloud-sun, cloud-moon, cloud, rain, showers, snow,
  fog, storm; `firmware/tools/gen_weather_icons.py`) plus the temperature; the words remain on the
  night page. Web assets are referenced with their content hash so a browser never keeps an old
  page after an update.
- The optional HTTPS mode (`device.use_https`, `device.tls_verify`, the CA bundle) is gone: it
  was off by default, the board cannot afford a TLS session's RAM, every service the firmware
  uses serves plain http, and dropping it frees ~100 KB of flash. Old configs with those keys
  still load.

## v0.1.1 - 2026-09-14

- Crowding: the word for SEPTA's "many seats" level is now `open` (it used to be a bare `seats`,
  which read as a truncated label). `device.show_crowding` becomes `device.crowding` (off / words /
  icons / both) plus `device.crowding_icons` (seats = chairs then people, crowd = people only); old
  configs are migrated on load. The web Now page shows the same word or meter.
- Crowding icons were invisible on the panel (the recolor colour prefix lost its trailing space
  to a too-small buffer, so LVGL laid the label out at zero width); the person glyph is now a
  wider silhouette. `/api/debug/ui` reports each row's label state (`rows`).
- Indego: a proper section on the panel (`[bicycle] Indego` header with a stale-age note, one row
  per station with bicycle / bolt / dock counts, red at 0 and amber at 1-2) and `bike.style`
  (icons | words). `/api/state` bike stations gain `classic` and `total_docks`; the web Now page
  card matches.
- Flasher page lists every service the firmware talks to.

## v0.1.0 - 2026-09-14

First release. Verified on an ESP32-3248S035R (3.5" resistive Cheap Yellow Display); the other
five Sunton variants build in CI but have not been tested on hardware.

- Live SEPTA bus/trolley arrivals from GTFS-RT joined with TransitView lateness; schedule rows
  from BusSchedules with the wrong-service-day defence; Regional Rail via the Arrivals API;
  subway schedule-only.
- Service alerts and detours ticker (height, speed, and what it shows are settings).
- Weather from Open-Meteo in the header and per-stop notes when the forecast at the next arrival
  is worth mentioning.
- Time-to-leave alert: blinking row, LED, optional speaker chime.
- Crowding (SEPTA seat estimate), commute profiles, alternative stops on long waits, quiet hours
  with wake-on-touch, night clock page, large-text mode, per-stop titles, light and dark themes,
  rotation.
- Indego bike-share counts for up to three stations.
- SD-card logging of predictions and arrivals with on-device statistics (on-time %, lateness by
  hour and weekday, headway/bunching, ghost buses, prediction accuracy) and CSV download.
- Web UI served by the device: stop wizard with map, stats charts, every setting, OTA update.
- Wi-Fi setup hotspot, mDNS (`transit-display.local`), browser flasher on GitHub Pages.
