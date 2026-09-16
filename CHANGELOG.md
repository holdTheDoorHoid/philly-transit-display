# Changelog

## Unreleased - release-candidate fixes

Seven defects found reading the v0.3.0 release candidate end to end. Four of them could restart or
boot-loop a display in someone's home; the rest are safety nets that were not quite doing what
their comments said.

- **The new "restart if fetching has stopped" safety net could restart a perfectly healthy display,
  over and over.** It judged the display by whether a whole round of fetching had *finished*, and
  gave that five minutes. But on a network that has gone quiet without saying so - the internet
  down while the Wi-Fi still works, a hotel or café sign-in page, a bad connection - one round can
  legitimately take far longer than that: every request waits out its own timeouts, and there is
  one request per stop, several times over. A display with six or more stops on such a network
  could never finish a round at all, so the net restarted it mid-round, forever, until the network
  came back. Two changes: the display now also reports in every time it *starts* a request, not
  only when it finishes a round, so "is it still going?" is answered by what it is actually doing;
  and three layers of the fetching code that each retried the same dead request independently now
  stop as soon as the network itself is unreachable, instead of multiplying (one stop's timetable
  could cost twelve full attempts a round; it now costs one). The retrying that exists for a real
  reason - SEPTA sometimes answering with the wrong day's timetable - is untouched. A display with
  the maximum eight stops on a blackholing network now keeps working through it, and a display that
  has genuinely frozen is still restarted within about five minutes, whatever its stop list.
- **Firmware uploads could restart the display instead of installing.** Two separate things. The
  groundwork that makes the device survive running out of memory has to be done once per internal
  worker, early, and the upload path ran before the place that does it — so on a display whose
  first-ever web request was a firmware upload, running out of memory during that upload was fatal
  rather than a polite refusal. And the older memory safety net had no pause for uploads, so a
  display already near its threshold could restart itself part-way through writing the new image.
  Neither could damage a display (the new image is only switched to once it has arrived complete),
  but both threw the upload away at the worst possible moment. Both fixed.
- **The check that stops a hostile web page from reaching your display now runs before the display
  writes anything, not just before it answers.** For a firmware upload it used to run afterwards,
  so a rebound request was refused — but only after the device had already written up to 1.7 MB to
  its spare memory. Not something an attacker could actually reach (uploads need the PIN, which a
  web page cannot supply from another site), but the check is worth having where it claims to be.
- **A display could boot-loop on a stop list it had happily accepted.** Stops can each ask for one
  to four arrival rows, and a four-row panel takes about three times the drawing memory of a
  one-row one. The guard that stops the arrivals page running out of memory measured the biggest
  panel it had built so far and assumed the next one was no bigger — so a list that happens to put
  the small stops first could let a big one in with nowhere near enough room. Since which stops are
  shown changes by time of day, the same settings could be fine in the morning and crash in the
  evening — and because it crashes while drawing the first screen at switch-on, with the settings
  still saved, the only way out was a USB cable. The guard now scales its estimate by how many rows
  the next panel actually wants, and the "N more stops will not fit" line at the bottom is now
  reserved up front rather than squeezed out of what is left.
- **The display no longer waits on the fetching to draw itself - anywhere - and the crash that
  caused is gone.** An earlier fix in this same release made the Device info screen stop waiting on
  the fetcher, and was written up as if that were the whole problem. It was one instance of it.
  Testing the release candidate found the same pattern in seven more places and crashed a display
  through one of them: the bike
  counts on the arrivals page asked the fetcher for its numbers, waited half a second for an answer
  while the fetcher was busy, and the display restarted. The weather in the header, the weather note
  under each stop, the SD card line, the statistics and the arrivals themselves were all still
  written the same way; only two had been fixed. Now the screen never waits for the fetcher at all -
  it either gets an answer immediately or redraws the one it had a second ago. The most a busy
  moment can cost is one second of staleness on one line, and there is no wait left that could turn
  into a restart. That includes the two places the earlier fix had already touched: shortening the
  wait turned out to make this particular crash *more* likely rather than less, so "waits a
  twentieth of a second" was not a smaller version of the fix, and they now wait not at all like
  everything else.

  The other half of the same fix is that the fetcher stops making the screen wait in the first
  place. It used to copy the whole list of arrivals twice - once to hand it over, once for the
  screen to pick up - with the screen locked out for the duration of both. It now hands over a
  reference instead of a copy. The screen is quicker, and the display has meaningfully more memory
  free from one minute to the next, which is the thing that was making the web page answer "busy,
  try again" after a few hours up.
- **When the display restarts because it ran out of drawing memory, it now says so.** That restart
  used to be indistinguishable from an ordinary one, which is unhelpful precisely when it matters:
  a stop list that does not fit crashes the same way on every switch-on, so what you have is a boot
  loop with no explanation. The web page can now ask why it last restarted and get "ran out of
  screen memory", with the numbers.
- **The web page no longer gets stuck on "Connecting to the display…" forever.** It retries a busy
  display indefinitely, which is right — a busy display answers in a second or two. But it did the
  same for answers that will never change, the common one being opening the page at the display's
  *old* name after renaming it. Those now stop, and say what happened and what to do about it.
- Documentation: four memory thresholds in DESIGN.md still quoted the pre-release round numbers
  after the code had moved to exact ones, and two places disagreed about how many internal workers
  the device has. Both corrected, with a note that thresholds are quoted in bytes from now on.

## v0.3.0 - 2026-09-16

- **The display now notices when it has quietly stopped fetching arrivals, and restarts itself.**
  It already restarted itself when its memory got so chopped up that every fetch failed — but that
  safety net worked by counting *failed* fetches, so it could only see a display that was still
  trying. If the fetching stopped dead instead (stuck deep inside the network code, which is what
  happened on a test board on 16 September), nothing was counting, nothing complained, and the
  screen sat there showing arrival times that were quietly getting older and older until the board
  eventually fell over on its own. Now the part of the display that draws the screen keeps an eye
  on the part that fetches, and if no fetch has finished — successfully *or* unsuccessfully — for
  several times the normal gap between them, it restarts. It waits at least five minutes before
  doing that, waits longer if you've set a slow refresh, holds off entirely while a firmware update
  is uploading, and gives itself extra time just after switching on. A display that simply has no
  internet is *not* affected: a fetch that fails is still a fetch that finished, so nothing
  restarts and it keeps retrying as before. Your settings are saved on the device, so a restart
  loses nothing. If it does happen, the display says so afterwards — the web page can now ask it
  why it last restarted, and it answers in plain words with the numbers involved.

- **Tapping through the screens no longer risks restarting the display.** The screen has its own
  small, fixed pot of drawing memory, separate from everything else, and the old way of switching
  pages built the next screen before letting go of the one it was leaving - so for a moment it was
  holding two or three at once. On the owner's own two stops, the second tap (from Statistics to
  Device info) asked for more than was left. Each page is now let go of before the next one is
  built, so only one is ever held, and there is room to spare on every configuration. The arrivals
  page also comes back to the front by itself if a page ever cannot be drawn, so a tap can never
  leave the display stuck on Statistics or Device info.
- The display can hold about four stops on the 3.5" screens and six on the 2.4"/2.8" ones. Asking
  for more used to be accepted and then fail; the arrivals page now shows as many as fit and says
  `2 more stops will not fit in this display's memory` at the bottom, so nothing disappears
  silently and the display keeps working.
- Web page: it used to go completely blank if you opened it (or it was already open and
  quietly refreshing in the background) at the moment the display was too busy to answer -
  something that happens on purpose several times a minute, by design (see the HTTPS/memory
  note below), and isn't a sign anything is wrong. Now the page always keeps showing whatever
  it last knew, quietly tries again a few times over the next several seconds, and shows a
  small "the display is busy right now, retrying..." note at the top instead of going blank or
  showing a scary red error. The very first time you open the page, if the display happens to
  be busy right then, you get a calm "Connecting to the display..." message instead of nothing.
  On the Settings page specifically: anything you've typed is never thrown away by this, and if
  Save happens to land at a busy moment you're told to just press it again rather than the
  change silently vanishing.
- Firmware updates over the web no longer get turned away while the display is busy, and the
  memory figures the device reports are now the honest ones. The device has two kinds of free
  memory: a region only usable in whole-word chunks, which nothing here can put a buffer or a
  piece of text into, and the ordinary kind, which is what everything actually uses. Every safety
  check and every number on screen had been counting both together, which made the device look
  like it had 33 KB more room than it really had. Three consequences, all now fixed:
  - **Updating over the web used to fail for no good reason.** On 2026-09-16 every attempt was
    refused for ten minutes while the display was fetching arrivals, and plugging in a cable was
    the only way through. The check was demanding a 16 KB run of free memory when the update
    actually needs 4 KB, and its other half moved up and down with the arrival fetches. Measured
    across 114 samples of normal operation, updates would have been refused 34% of the time
    before, and 4% now - and the 4% that remain are moments when memory is genuinely tight, which
    is what the check is for. Confirmed on the device: four uploads attempted in the exact window
    that used to be refused were all accepted.
  - **Two other safety checks had quietly not been running at all.** The ones protecting the
    status page and the background statistics work were set to thresholds that the miscounted
    figure could never fall below, so half of each check was dead. They now use the real number,
    at levels worked out from what each job actually needs. One visible result: while a firmware
    update is uploading, the web app's status data pauses for a few seconds and then resumes. That
    is the repaired check doing its job - during an upload it steps back so the update gets the
    memory it needs, because a failed update costs far more than a status reading arriving a few
    seconds late. The old build carried on answering through an upload only because the check was
    broken, so this is the bug going away rather than anything being taken out. The web app rides
    it out and keeps showing the last reading; if you have your own script polling the device,
    have it retry rather than treat it as an error.
  - **"Heap free" in the web app and on the device's own info page** now show the memory that can
    really be used. Expect this to read about 33 KB lower than before; nothing got worse, the old
    number was just counting memory that was never available. Anything reading the device's data
    feed sees the original figure unchanged, with the honest one added alongside it.
  - **If an update is ever refused, the message now tells you what to do about it** - restart the
    display and upload again within the first minute - and reports exact byte counts instead of
    rounding to the nearest kilobyte. The rounding mattered: a v0.2.0 device that was twelve bytes
    short of the old limit reported itself as "15 KB below 16 KB", which reads like something is
    using up a whole kilobyte rather than like a coin toss.

  **If you are running the released v0.2.0 on another device, read this.** On the owner's stop
  configuration, v0.2.0 settles twelve bytes below the old update limit and then refuses every
  update over the web, indefinitely. Restarting it clears the way, but only for about 45 seconds
  before it settles back, so the upload has to follow the restart immediately. Updating over the
  USB cable always works and is the reliable escape. Devices updated to this release are not
  affected - the new limit has real room in it rather than twelve bytes. Whether any particular
  v0.2.0 device hits this depends on its stops and which feeds it has switched on; it was measured
  on one configuration, not predicted for all of them.
- HTTPS to the data sources (SEPTA, Open-Meteo, Indego), measured on the owner's board and kept
  as a prototype. A firmware built with `-DTRANSIT_HTTPS` (the `cyd-*-https` envs) asks for a
  verified, encrypted connection on every fetch, with each source pinned to its one root
  certificate, and falls back to plain HTTP only when the device's memory cannot hold an encrypted
  session right now - never because a certificate failed to verify, which simply fails the fetch
  and is retried on the next poll. The web app shows which one the latest arrivals came over
  ("Data link" on the Now page) and offers the choice under Settings, Data & weather ("Data
  connection"); both appear only on a firmware that actually has HTTPS in it.
  **Shipping builds are unchanged and keep fetching over plain HTTP**, because over 21 polls on
  the owner's board the encrypted connection was never once affordable: all 64 fetches that asked
  for it were turned down, with about 29 KB of usable memory free against the 68 KB a session
  needs. The memory the device *reports* as free overstates what a buffer can actually use by
  about 34 KB - a region of the chip that only whole-word reads can reach - and the boot log and
  the poller heartbeat now print both figures side by side. We also checked whether simply doing
  the encrypted fetch at a quieter moment in the poll would help: the whole poll cycle is flat to
  within 6 KB, so the best moment is still about 27 KB short, and at that moment the two 16.7 KB
  buffers an encrypted session needs cannot both be found even with every other cost set to zero.
  DESIGN.md 2.1 records the numbers and what it would take (a rebuilt vendor library with smaller
  encryption buffers, or a board with extra memory). Nothing about the arrivals changed during the
  test: both stops stayed live and the poll cadence was the same as the shipping build.
  The Open-Meteo and Indego addresses are now written as `https://` like SEPTA's, which changes
  nothing in a shipping build. The HTTPS build also gives the poller task 2 KB more stack and
  prints a per-stage memory trace.
- Firmware update over the web: the curl example needed `-H "Expect:"`; without it the upload is
  dropped and the device stays on its old build (documented in firmware/README.md).

- Memory: the firmware now gives the C++ runtime a 2 KB emergency exception pool, so an
  out-of-memory error can always be caught and answered with a 503 or a skipped frame instead
  of rebooting the device (this used to be the one uncatchable case, DESIGN.md 12.1). Done by
  supplying the pool-size hook from the firmware itself - no SDK rebuild. `POST /api/debug/oom`
  (PIN) proves it on the device: it drains the heap, forces the failure, and reports
  `caught:true`. One gap in that turned up the same day and is fixed too: the very first
  out-of-memory error on each background task needed a scrap of memory of its own, which is not
  there by definition at that moment, so on a freshly booted device the first failure still
  rebooted it. Each task now claims that scrap at startup while memory is plentiful, and the
  on-device check runs right after a reboot so it actually tests the first-failure case.
  `/api/state` and `/api/config` are streamed straight into the network buffers instead of being
  built as a second copy first, lowering the peak memory of the busiest request.
- Screen: the Wi-Fi signal in the top strip is four bars filled in solid, like a phone's status
  bar, instead of a symbol with a number beside it. The same bars appear on the device page.
- Screen: the statistics page now looks like part of the arrivals page - the same top strip, one
  card per stop with its route badge and title - and shows the on-time percentage as a big
  number over a filled bar (green, amber or red), with the average lateness in the arrival
  badge colours, the worst hour and the ghost count as labelled tiles, and the arrival count
  underneath. The card adapts to every board: two stops on the 3.5" board get the full layout,
  four stops on a 2.4" board get one line each, nothing is clipped.
- Screen: the device page is arranged as a Network card (signal bars, network name, the web
  address, IP address, and whether the last SEPTA update worked) and a card named after the
  device (the web PIN in large digits, SD card status - with a warning when log rows are not
  being written - free memory and uptime), with the hold-to-reset-Wi-Fi button along the bottom.
  Where the screen is tall enough (the 3.5" board in either orientation) a third card, Data
  sources, lists each feed that is switched on - SEPTA, weather, Indego, service alerts - with
  ok / stale / failed in the arrival colours and how long ago it was last fetched, so "is it
  working?" has a one-glance answer. These two pages are now built when you tap to them and freed when you leave, so a four-stop
  setup fits in the display's memory.
- Web: the Settings page is arranged into blocks of settings that affect each other (Screen &
  appearance, Schedules, Alerts & reminders, Data & weather, Device & network, Web PIN,
  Firmware & maintenance) with a one-line intro each; a setting that only matters when another
  is on sits under it and is dimmed while it is off; jump pills and a "Filter settings" box find
  a setting by name or description; one sticky save bar shows unsaved changes with Undo; reboot
  and Wi-Fi reset sit in a fenced "Restart and reset" area with plain-language hints.

## v0.2.0 - 2026-09-16

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
