# Changelog

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
