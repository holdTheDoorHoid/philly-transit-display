# Changelog

## Unreleased - 2026-09-15 review fixes

An external adversarial review of v0.1.2 (32 findings) drove this pass. HTTPS to SEPTA remains
deferred to its own release; firmware signing was deliberately not added.

- Security: state-changing requests (save settings, reboot, Wi-Fi reset, firmware update, PIN
  change, log downloads) need a per-device PIN sent as `X-Pin`; viewing stays open. The PIN is
  generated on first boot, printed once on the serial console and shown on the device info screen;
  change it from Settings. Five wrong tries lock the routes for 30 s. Requests with a foreign
  `Host` header are refused (DNS-rebinding defence); the page cannot be framed.
- Setup Wi-Fi: the first-boot network now has a per-device WPA2 password shown on the screen with
  a QR code. A device that already has Wi-Fi saved never reopens setup by itself: it keeps retrying
  its network and only a tap on the screen opens setup, which closes again after 10 idle minutes.
  Setup rejects overlapping connection attempts and escapes SSIDs correctly.
- Firmware updates: an upload without a file, a second concurrent upload, a disconnect mid-file,
  an oversized image, too little or too fragmented heap, and an image built for a different board
  are each refused with a specific error; the device only reboots after the image verified.
- Settings: every value is type- and range-checked before it is narrowed (256 no longer wraps to
  brightness 0); strings are length-capped and control characters rejected; profile stop lists
  must be unique; Regional Rail lines are normalised to their code (`Paoli/Thorndale` becomes
  `PAO`). Saves go through a temporary file, are read back and verified, and the previous file is
  kept as `/config.prev.json` for recovery (`config_recovered` in `/api/state`). The device name
  and time zone apply immediately on save. The panel honours each stop's `show` count.
- Arrivals: a shared stop no longer shows another route's schedule (subway aliases still work);
  skipped and cancelled trips no longer reappear as scheduled arrivals; truncated or malformed
  feeds, a failed schedule for one stop and a failed rail station now report per-stop health
  (`live`, `schedule_only`, `stale`, `unavailable`) instead of looking like "nothing due"; a feed
  older than five minutes is marked stale rather than shown as live. Every hand-written number
  parser is bounded (a malformed rail status used to overflow). Retained feed data is capped.
- Statistics: a failed poll can no longer create arrivals, ghosts, no-shows or headways; a
  scheduled trip that turns live is reconciled instead of later counted as a no-show; the
  tracker keeps the soonest trips rather than churning; headways are computed in time order and
  never negative; on-time % is computed over arrivals with known lateness (null when none) with
  the counts shown; "prediction accuracy" is now "forecast stability" and arrival rows record how
  they were inferred; outages count from the first failure and include ongoing ones; the
  overview reserves slots for the currently configured stops and reports exclusions.
- Log file: the header now matches the 21-column rows (schema v3, `temp_c` in Celsius); records
  are bounded so the reader can always read what the writer wrote; blank lines are skipped;
  downloads re-emit a mixed month in one schema with spreadsheet-safe text; short SD writes are
  counted (`sd.dropped_rows`, `sd.write_ok`) and a removed card is reported; one download at a
  time so the poller keeps its file handle.
- Poller: arrivals are published as soon as they are fetched; alerts, weather, Indego and logging
  run afterwards on their own timers and cannot delay the next poll; a peer trickling bytes is cut
  off by an absolute deadline; proxied setup responses are leased until the browser finished
  reading them; the on-device stats page no longer scans the SD card on the display task.
- Weather and Indego: failed weather fetches back off (1, 5, 10 min) and a forecast older than an
  hour is hidden rather than shown as current; Indego rows are only logged from a fresh sample and
  unknown counts stay unknown.
- Web: the stop-map tooltip no longer renders agency text as HTML; Leaflet loads with subresource
  integrity; the wizard offers Regional Rail lines by name but stores the code, and asks bus vs
  trolley explicitly; a live prediction without lateness data is labelled "live", not "sched";
  the Now page shows each stop's health; the Indego request uses HTTPS; every setting has a
  plain-language hint (including what each crowding word and icon means); the PIN is asked for
  once and remembered by the browser.

## v0.1.2 - 2026-09-15

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
