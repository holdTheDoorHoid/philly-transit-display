# Philly Transit Display — web UI

Vanilla HTML/CSS/JS single-page app served by the ESP32 from flash. No framework, no
bundler, no npm dependencies — only `index.html`, `app.js`, `app.css`, `favicon.svg`,
plus two Node scripts for development and embedding. See `DESIGN.md` §6–10 for the
full contract this UI is built against.

## Running against the mock server (no hardware needed)

```
node web/mock-server.mjs            # listens on :8080
node web/mock-server.mjs 8090       # or a specific port
```

Then open `http://localhost:8080/`. **The mock device's PIN is `123456`** — the UI asks
for it the first time you save something (see "Admin PIN" below).

The mock server implements every route in
DESIGN.md §7 using Node built-ins only (`node:http`, `node:fs`, `node:path`,
`node:zlib`* , `node:crypto`, `node:url`) and the real SEPTA fixtures in
`firmware/test/fixtures/`:

- Serves `index.html` / `app.js` / `app.css` / `favicon.svg` **raw** (ungzipped) with
  `Cache-Control: no-store`, so edits show up on refresh with no rebuild step.
- `GET /api/state` — builds a live-looking snapshot from the fixtures with all times
  computed relative to *now*: route 17 both directions (southbound stop 21332,
  northbound stop 21297) with one `live`, one `scheduled`, and one `skipped` arrival
  row each, plus a Regional Rail (30th Street, northbound) panel from
  `arrivals_30th.json`. Add `?stale=1` to simulate a failed poll (amber "stale" banner
  in the Now view), `?recovered=1` to set `config_recovered` (the Settings page's
  "restored its previous settings" notice). One alert (from `alerts_bus_17.json`) is
  included with its HTML advisory text stripped to plain text. Also carries
  `auth: { pin_required: true }`, `board: "cyd-3248S035R"` and `config_recovered`.
  Each stop carries `health` (`live` | `schedule_only` | `stale` | `unavailable`) and
  `source_age_s`: southbound 17 is `live`, northbound 17 is deliberately `stale`
  (480 s, so the Now page shows a "stale 8 min" chip), and the rail panel is
  `schedule_only`. The northbound `live` arrival deliberately has `late_known: false`
  — a live prediction with no lateness figure, which is the case the Now page has to
  label "live" rather than "sched".
- `GET`/`PUT /api/config` — in-memory config seeded with the exact example from
  DESIGN.md §6 (routes 17 southbound/northbound + Regional Rail north). `PUT`
  validates server-side and returns `400 { error, path }` on the same rules described
  in §6 (ranges, required fields, mode-specific fields, duplicate/too-many keys).
- `GET /api/proxy/stops?route=17` returns `stops_17.json` verbatim (133 stops); any
  other route returns `200 []`, matching SEPTA's real behavior for an unknown route.
- `GET /api/proxy/schedule?stop_id=` returns `busschedules_21332.json` /
  `busschedules_21297.json` verbatim for those two stops; any other `stop_id`
  gets a synthesized-but-plausible response (see "Assumptions" below) so the Add
  Stop wizard can be exercised against any of the 133 stops on route 17.
- `GET /api/rail/stations` returns `firmware/test/fixtures/rail_stations.json` if
  that file exists (it doesn't yet — that fixture belongs to another agent), else a
  ~55-station built-in list covering all Regional Rail lines.
- `GET /api/stats?stop=&days=` returns deterministic synthetic aggregates (same
  `stop`+`days` always produces the same numbers) with the firmware's final key names:
  `samples`, `inferred`, `unobserved`, `late_known`, `on_time_pct` and `mean_late_min`
  (both JSON `null` when `late_known` is 0), `by_hour`, `by_weekday`, `headway`,
  `ghost`, `noshow`, `outage_min` (includes ongoing/partial outages),
  `forecast_stability[]` (the renamed `prediction[]`, per-bucket keys
  `mean_abs_revision_s` / `mean_revision_s` replacing `mae_s` / `bias_s`),
  `crowding: { by_hour, by_weekday }`, `wait_by_hour`, `wait_basis`
  (`"half_mean_gap"`), `coverage` (0..1 fraction of the window polled successfully).
  `unobserved` tracks `1 - coverage` so the two agree. Add `&empty=1` for the
  zero-sample "no data yet" shape, or `&nolate=1` to force `on_time_pct: null` so the
  UI's "no lateness data" wording can be checked — rail stops always take that path.
- `GET /api/stats/overview?days=` — one summary row per configured stop (`stop`,
  `samples`, `inferred`, `late_known`, nullable `on_time_pct` / `mean_late_min`,
  `ghost`, `noshow`, `outage_min`, `coverage`, `last_seen_ts`) for the Stats page's
  all-stops table, plus `bikes` (per-station `samples` and `by_hour` Indego
  availability, empty when `bike.enabled` is off), top-level `samples` and `inferred`,
  `excluded_stops` (2, standing for stop keys still in the logs that the fixed-size
  aggregator had no room for), `excluded_bikes` and `coverage`. The rail stop's row
  deliberately has `on_time_pct: null` so the table's "no data" path is visible. A
  stop's row reuses `buildStats`'s own PRNG sequence so its numbers match what you see
  after selecting it in the per-stop detail below.
- `GET /api/log/index` (open) / `GET /api/log/<file>` (**PIN-protected**) — a
  synthetic two-file index and CSV content in **log schema v3**:
  `ts,event,stop_key,route,dir,trip,sched_trip,vehicle,scheduled_ts,predicted_ts,`
  `actual_ts,late_min,horizon_s,headway_s,seats,temp_c,note`. Temperature is
  `temp_c`, in Celsius; anything that displays it converts to the device's own unit
  setting first.
- `POST /api/ota` (**PIN-protected**) drains the multipart body and replies after a
  short simulated delay. A second upload while one is in flight gets `409`, and an
  upload whose filename contains `wrongboard` gets `400` with a board-mismatch
  message, so the UI's wording for both can be checked.
  `POST /api/reboot` and `POST /api/wifi/reset` (**PIN-protected**) reply `200`.
- `POST /api/pin` (**PIN-protected**) takes `{"pin":"new"}` (4-32 printable ASCII, no
  spaces) and changes the PIN for the rest of the process's life.

### Admin PIN

The mock mirrors the firmware's gate so the unlock flow can be exercised without
hardware. Everything that changes the device — `PUT /api/config`, `POST /api/reboot`,
`POST /api/wifi/reset`, `POST /api/ota`, `POST /api/pin` — plus `GET
/api/log/<file>.csv` wants an `X-Pin` header:

| Condition | Response |
|---|---|
| header absent | `401 {"error":"pin required"}` (does not count as an attempt) |
| header wrong | `401 {"error":"wrong pin"}` |
| 5 wrong in a row | `429 {"error":"too many attempts","retry_s":60}` for 60 s |

The starting PIN is **`123456`**; restart the server to reset it (and to clear a
lockout). The browser remembers whatever you type in `localStorage` under `ptd_pin`;
`localStorage.removeItem('ptd_pin')` in the console, or the Settings page's "Forget
PIN on this browser" button, puts you back to the first-save prompt.

\* `node:zlib` is only used by `build.mjs`; the mock server itself doesn't gzip
anything (see above).

### Fault injection

DESIGN.md §12.1: `GET /api/state` and `GET /api/config` refuse with a `503` when the
device is short on heap mid-poll, and `/api/state` can additionally come back as a
genuine empty `200` (a send-buffer artifact, not an application error). The mock can
reproduce both — plus a slow response and a dropped connection — on a chosen fraction of
requests, off by default, so the web UI's retry/backoff/never-blank behavior (see
DESIGN.md §12.1 and the web changelog) can be exercised without hardware:

```
MOCK_FAULT_RATE=0.4 node web/mock-server.mjs      # env vars, fixed for the process
MOCK_FAULT_MODES=503,empty node web/mock-server.mjs   # default: 503,empty,slow,reset

curl http://localhost:8080/__test__/faults                                # read it live
curl -X POST http://localhost:8080/__test__/faults \
  -d '{"rate":0.4,"modes":["503","empty","slow","reset"]}'                # change it live,
                                                    # no restart -- so a page that's
                                                    # already loaded and polling can be
                                                    # driven through "busy" and back to
                                                    # "recovered" in the same session

curl 'http://localhost:8080/api/state?fault=503'    # force just this one request
```

`paths` (default `/api/state,/api/config`) and `slowMs` (default `6000`) are settable
the same two ways. `modes`: `503` (the documented refusal), `empty` (the documented
empty `200`), `slow` (delays `slowMs` then answers normally — under the web UI's 8 s
client-side timeout it just looks slow; over it, it looks like a hung connection), and
`reset`/`network` (destroys the socket, like a device that dropped off Wi-Fi mid-request).

## Rebuilding `firmware/src/generated/web_assets.h`

```
node web/build.mjs           # gzips the 4 assets, (re)writes the generated header
node web/build.mjs --check   # exits non-zero if the committed header is stale (CI)
```

The header contains, per asset, a `PROGMEM` byte array (`<NAME>_GZ`) and its length,
a `WebAsset` table (`path`, `content_type`, `data`, `len`) the firmware's web server
iterates over to serve `/`, `/app.js`, `/app.css`, `/favicon.svg` with
`Content-Encoding: gzip`, and a `WEB_ASSETS_ETAG` (first 16 hex chars of a SHA-256 of
the concatenated raw sources) for the `ETag` header. `PROGMEM` is guarded with
`#ifndef PROGMEM #define PROGMEM #endif` so the same header compiles unmodified on
the host (verified with `g++ -std=c++17 -Wall -Wextra`) and on the ESP32 toolchain.

Run `node web/build.mjs` again any time `web/index.html`, `web/app.js`, `web/app.css`,
or `web/favicon.svg` change, and commit the regenerated header alongside them — CI
runs `node web/build.mjs --check` and fails the build if you forget.

## Size budget

DESIGN.md §10 caps the four assets at **60 KB gzipped total**. Current sizes
(`node web/build.mjs` prints these on every run):

| Asset | On disk | Embedded | Gzip |
|---|---:|---:|---:|
| `index.html` | 912 B | 950 B | 476 B |
| `app.js` | ~160 KB | ~136 KB | ~36.6 KB |
| `app.css` | ~18.7 KB | ~15.9 KB | ~3.9 KB |
| `favicon.svg` | 410 B | 410 B | 202 B |
| **Total** | **~179 KB** | **~153 KB** | **~41.1 KB** |

That leaves roughly 18 KB of headroom under the budget. `build.mjs` prints a warning
(without failing) if the total ever exceeds 60,000 bytes gzip.

**"Embedded" is smaller than "on disk" because `build.mjs` strips whole-line comments
from `app.js`/`app.css` before gzipping** — about 16 KB of source comments, worth
~4.8 KB gzipped, which is what pays for commenting the files at the density the rest of
the repo uses. `web/` on disk, the mock server, and anything you read or debug locally
are untouched; only the copy baked into flash is trimmed. `index.html` goes the other
way (886 B embedded vs 848 B on disk) because the cache-busting `?v=<etag>` query
strings are added after the strip.

The stripper is deliberately conservative — a line only goes if it is *entirely* a
comment, so a trailing `// note` after code survives — and it is guarded twice:
`assertNoMultilineStrings()` refuses to run if the source ever grows a template literal
spanning lines (the one construct a whole-line rule could corrupt), and `assertParses()`
compiles the result with `new Function` before it is embedded. To read exactly what ends
up on the device, gunzip `APP_JS_GZ` out of the generated header.

## How the UI maps to the device API

| View | Reads | Writes |
|---|---|---|
| **Now** | `GET /api/state` every 15 s | — |
| **Stops** | `GET /api/config`, `GET /api/proxy/stops`, `GET /api/proxy/schedule`, `GET /api/rail/stations` | `PUT /api/config` (reorder, edit, remove, add stops; enable/add/remove Indego stations — always sends the whole config) |
| **Stats** | `GET /api/config` (stop picker/labels), `GET /api/stats/overview` (all-stops table + Indego charts), `GET /api/stats` (per-stop detail), `GET /api/log/index` | `GET /api/log/<file>.csv` (PIN, fetched as a Blob) |
| **Settings** | `GET /api/config`, `GET /api/state` (firmware version, board, `config_recovered`, `auth`) | `PUT /api/config`, `POST /api/ota`, `POST /api/reboot`, `POST /api/wifi/reset`, `POST /api/pin` (all PIN) |

Every write, plus the CSV downloads, goes through the PIN wrapper: `fetchJSON(url,
opts, true)` adds `X-Pin` from `localStorage.ptd_pin`, and a `401` opens the modal
("This device asks for its PIN before changing settings…"), stores what you type and
replays the request once. A second `401` says the PIN was not accepted rather than
echoing the firmware's `wrong pin`; a `429` reports the wait from `retry_s` and does
**not** retry. `uploadFirmware` does the same through `XMLHttpRequest`
(`setRequestHeader('X-Pin', …)`), and words `409` as "already installing an update"
and `400` with the firmware's own message (wrong board, bad image). The CSV links are
buttons, not `<a href>`: a plain link cannot carry the header, so `downloadLog()`
fetches the bytes and hands them over as a Blob + object URL + synthetic click.

The Now view polls `/api/state` every 15 seconds while active and stops polling when
you navigate away (hash routing: `#/now`, `#/stops`, `#/stats`, `#/settings`).

The Stops → Add flow is a small state machine (`wizard` object in `app.js`):
mode → route/station lookup → pick a stop (list + optional Leaflet map, loaded at
runtime from `unpkg.com`; the UI degrades to list-only within ~6 s if the CDN is
unreachable) → direction (learned from `/api/proxy/schedule`) → label/rows → save.
`key` is generated client-side as `<route>-<stop_id>-<direction>` for bus/trolley/
subway, or `rail-<station-slug>-<N|S|both>` for Regional Rail, and the whole config
object is sent to `PUT /api/config` on save; a `400` response's `{ error, path }` is
shown inline in the wizard.

Leaflet's CSS and JS are pinned to 1.9.4 and loaded with Subresource Integrity plus
`crossorigin="anonymous"` (`LEAFLET_*_SRI` in `app.js`; the comment there records the
`curl … | openssl dgst -sha384 -binary | openssl base64 -A` command that produced the
hashes). A hash mismatch makes the browser drop the file, the loader times out, and
the wizard falls back to its list-only view — the same path as being offline. Stop
names from SEPTA go into Leaflet tooltips as an element built with `h()`, never as a
string: Leaflet renders string tooltip content through `innerHTML`.

The route step has a **Service type** select (Bus / Trolley) that sets the stop's
`mode`. It guesses from the route id as you type — `/^(T\d|G1|D\d|10|11|13|15|34|36|
101|102)$/` means trolley — and stops guessing once you change it yourself. The
stop edit form carries the same select, so a stop the old wizard saved as `bus` can
be corrected in place.

Regional Rail's line filter is a **dropdown of SEPTA line codes**, not free text: the
firmware matches on the code (`PAO`), never the public name ("Paoli/Thorndale"), so
the old text box produced a filter that silently matched nothing. The chosen code is
written to **both** `route` and `line` — DESIGN.md §6 documents rail `line` as mapping
onto `StopConfig::route`, and writing both leaves no stale display name behind.
Editing a stop that still holds a display name pre-selects the matching code
(`railLineCode()`), and anything unrecognised falls back to "Any line" rather than
inventing a filter.

Each configured stop's edit form (§6 "Fields added 2026-09-14") also sets how its
title is shown on screen (`title_style`: label → destination, label only, route →
destination • stop, or custom text saved as `title_text`) and whether the stop is an
*alternative* — hidden unless another configured stop's next arrival is more than
`alt_after_min` minutes away (`alt_of`, the other stop's `key`). The stop list preview
computes the on-screen title client-side the same way the firmware will, and shows an
"alternative to …" hint when `alt_of` is set.

Below the stop list, the Stops page also has an **Indego bikes** card (`bike`, §6,
max 3 stations): an enabled checkbox, an **Indego style** select (`bike.style`:
`icons` (default) or `words` — picks how counts are drawn on the Now page, see below),
the chosen stations with remove buttons, a "Find stations near my stops" lookup that
fetches the Bicycle Transit status feed directly in the browser and ranks the six
nearest stations by great-circle distance from stops that have `lat`/`lng`
(`https://bts-status.bicycletransit.workers.dev/phl` — verified 2026-09-15 to answer
over TLS with `Access-Control-Allow-Origin: *`, so it works whichever scheme the page
was loaded over and no longer needs the old "load the UI over http://" advice; a
failure now reads as "no internet connection right now"), and a
manual station-id add. It's on the Stops page rather than Settings because the owner
manages bike stations alongside the stops they sit near; every change there saves
immediately through the same `PUT /api/config` pattern as reordering or removing a
stop (whole config, `liveConfig` kept in sync, a "Saved." banner on success). This
card is hidden while the Add Stop wizard is open.

On the Now page, the Indego card (`renderBikeCard` in `app.js`) shows only when
`bike.enabled` and at least one station is configured: it uses the same `.card
.stop-panel` container and `.title` bar as a stop panel (bicycle pictogram + "Indego",
with the stale-age note right-aligned inside that same bar via the existing `.row
.between` utility, rather than stacked below it) so it reads as one more panel in the
stack, and each station is a row styled like `.arrival-row` (same padding/border as the
stop panels' arrival rows). `bike.style` isn't echoed on `/api/state`, so the Now
view reads it once from `/api/config` alongside the crowding settings and falls back
to `icons` if it's absent (old firmware) or unrecognized. In `icons` style each row
shows three original inline-SVG pictograms with a count next to each — bicycle
(classic bikes), bolt (e-bikes), dock ("P" in a rounded square, free docks) — drawn
the same way as the crowding glyphs (`crowdIcon`), not traced from any icon set; in
`words` style the same three counts are spelled out ("5 bikes, 2 e-bikes, 7 docks")
instead. Either way each count is colored red at 0, amber at 1-2, and the normal text
color otherwise (`.tone-red` / `.tone-amber` in `app.css`). A station with `bikes ===
-1` (missing from the feed) shows "no data"; one with `active: false` shows "offline"
(muted) — both hide the counts entirely. The mock's three default stations (Snyder &
Dorrance, 15th & Spruce, Girard Station) are fixed via `BIKE_DEMO` in
`mock-server.mjs` to cover all of the above in one screen: a 0/1-2/normal spread in
one row, plus a "no data" row and an "offline" row.

The Stats page opens with an **all-stops overview table** (`GET
/api/stats/overview?days=`, same 7/30/90 selector as the per-stop detail below it):
one row per configured stop with samples, on-time % (colored green/amber/red at the
same ≥80/60-80/<60 thresholds as elsewhere), mean late, ghosts, no-shows, and a
relative "last seen" time; clicking a row selects that stop in the detail picker
below (`buildOverviewTable`'s `onSelect` callback). An empty `stops[]` (nothing
logged yet) shows a muted "No arrivals logged yet." message instead of an empty
table. When the response's `bikes[]` is non-empty, one small chart per Indego station
follows the table (`buildBikeOverviewCards`): stacked bars of mean classic/e-bikes per
hour with a line for mean docks, hidden entirely when `bikes[]` is empty (Indego
disabled, or no stations configured).

Per-stop detail (`GET /api/stats?stop=&days=`) adds four chart cards after the
existing lateness/headway/prediction ones, all built with the same inline-SVG helpers
(`buildCrowdChart`, `buildWaitChart`, `buildReliabilityChart` in `app.js`) rather than
a charting library: **Crowding by hour** and **by weekday** (0-5 mean level per bin,
green ≤1 / amber 2-3 / red ≥4, hover a bar for the full 6-level distribution);
**Expected wait by hour** (bar = mean gap between buses in minutes, tick = worst gap
seen — "if you arrive at random, the typical wait is about half the mean gap"); and
**Reliability by hour** (stacked ghosts/no-shows per hour). All four skip drawing a
bar for hours/weekdays with no samples (`n === 0`) rather than drawing a zero-height
one.

### Settings page layout

Settings is one config form arranged as blocks of things that affect each other, in
this order: **Screen & appearance** (theme, brightness, rotation, invert, large text,
crowding and its icon scheme, the top-strip items), **Schedules** (quiet hours, night
clock, commute profiles — the three that decide what the screen shows *when*),
**Alerts & reminders** (service alerts → ticker contents → ticker height/speed; time to
leave), **Data & weather** (poll interval, SD logging, weather with its per-stop note
and units), **Device & network** (name, timezone), **Web PIN**, and **Firmware &
maintenance** (OTA upload, then reboot and Wi-Fi reset boxed off in a red "Restart and
reset" area). Each block is a `block()` card with a one-line intro; each control is a
`field()` (label + control + hint) so the filter can show or hide it as a unit.

A setting that only matters while another one is on sits in a `deps()` box under its
parent — indented behind a left rule — and `setDeps()` dims the box and disables
everything in it while the parent is off: the quiet-hours times, brightness and wake
time under the **Quiet hours** switch; the night-clock threshold under **Night clock**;
minutes, LED, screen and chime under **Time to leave**; the per-stop weather note and
units under **Show weather**; **Crowding icons** under a Crowding mode that shows icons;
and the ticker as two nested levels — the **Show service alerts** switch (`alerts`,
whether alerts are fetched from SEPTA at all) encloses the ticker **Show** select
(`device.ticker_show`: `both` (default), `alerts`, `detours`, or `off`), which in turn
encloses Height and Scroll speed, since `off` leaves nothing to size or scroll. Disabled
values are still collected and sent on save, so switching a parent off and on again
loses nothing.

Two sticky strips make the page navigable. At the top: one pill per block (jump links,
built as buttons rather than `#` anchors so they never fight the hash router, with the
block currently on screen highlighted) and a **Filter settings** box that hides every
field whose text — label, hint, option labels — does not contain what was typed
(case-insensitive), dims the pills of blocks with no match, and says "No settings match"
when nothing does. At the bottom: the one **Save settings** button and **Undo changes**,
both disabled until the form differs from what was loaded (a `JSON.stringify` compare of
`collect()` against the loaded config, run on every input/change/click under the form),
an "Unsaved changes" label while it does, and the saved/error banner shown in the bar
itself so it is visible where the click happened. Undo rebuilds the form from the last
loaded or saved config without a round trip; closing the tab with edits pending gets the
browser's own leave-page prompt. The PIN block and the Firmware block are not part of
the form — they talk to their own endpoints and keep their own buttons.

The **Crowding** select (`device.crowding`: `off`, `words`, `icons`, or `both` (default
`words`)) picks how SEPTA's per-bus seat estimate shows up next to each arrival on the
Now page — nothing, a word (open / few seats / standing / packed / full), a three-slot
icon meter, or icons followed by the word. The **Crowding icons** select
(`device.crowding_icons`: `seats` (default) or `crowd`) picks the icon scheme and is
dimmed and disabled except when Crowding is Icons or Icons + word. Old firmware that
only knows the boolean `device.show_crowding` still works: the UI reads `crowding` when
present and otherwise treats `show_crowding === false` as `off`, anything else as
`words`; it never sends `show_crowding` back. On the Now page, an arrival with no
recognized seats value (blank or `NOT_AVAILABLE`) shows no crowding element at all.

### Arrival badges and per-stop health (Now page)

`badgeFor()` reads the arrival's `status` first and `late_known` only after it, because
they answer different questions: `status` says whether the device has a live prediction,
`late_known` says whether SEPTA also supplied a lateness figure. `live` + `late_known:
false` therefore renders an outlined **live** badge with the title "live ETA, lateness
unknown", *not* "sched / no live tracking" — that older wording claimed the trip was
untracked whenever the lateness happened to be missing. `scheduled` → `sched`,
`skipped` → `skipped`, anything else → `sched`.

Each stop also carries `health`, `source_ts` (when SEPTA produced the data) and
`source_age_s`. `stopHealthChip()` draws a chip in the panel's title bar for `stale`
("stale 8 min", amber), `unavailable` ("unavailable: <error>", red) and `schedule_only`
("schedule only", grey); `live` and a missing `health` draw nothing, so a healthy panel
stays quiet. It prefers `source_age_s` and falls back to computing the age from
`source_ts`.

**`ok` is false for a stale stop as well as an unreachable one.** A stale stop still has
usable — if old — times, so `renderStopPanels` only replaces the rows with the red error
banner when the stop is *not* stale or has no rows at all; a stale stop keeps its
arrivals, gets the amber chip, and shows the short human `error` text as a muted line
above them. Treating `!ok` as "show nothing" would throw away the only information the
panel has.

A `status: "skipped"` arrival may carry `predicted: 0`; `fmtEta()` already falls back to
`scheduled` for its clock time. Arrivals also carry `sched_trip` (the matched static trip
id, possibly empty); the UI stores nothing and displays nothing from it today.

### Wording rules on the Stats page

Two rules the firmware's own honesty depends on:

1. **Never render a bare 0 for a missing value.** `fmtCount` / `fmtPct` / `fmtMinutes`
   turn `null` into `"no data"`. A stop with no lateness data is not a stop that was 0 %
   on time, and the all-stops table shows "no data" rather than a red 0 % chip.
2. **Always show the sample count beside a percentage.** The on-time tile carries "of
   143 with lateness data" and the sentence under the tiles reads "On time: 71 % of 143
   with lateness data (190 arrivals)."

Three more pieces of wording come straight from the data:

- **Arrivals are inferred**, not measured — the device works them out from the bus
  vanishing from the live feed. Said under the tiles, with the `inferred` count, and
  followed by `unobserved`: trips that fell in a stretch when the device was not polling,
  so nothing is known about them. `unobserved` is distinct from ghosts and no-shows,
  which are failures the device actually watched happen.
- **Forecast stability** replaces the old "prediction accuracy by horizon". The device
  has no measured arrival time to compute error against, so what it reports is how far
  the forecast was *revised* while the bus was in sight: `forecast_stability[]`, one
  bucket per horizon, bar = `mean_abs_revision_s`, dot = `mean_revision_s`. The
  bar-and-dot rendering is unchanged; only the labels are. Read defensively — an object
  carrying a histogram draws too, and the pre-rename `prediction[]` with `mae_s`/`bias_s`
  still renders under the new heading rather than leaving a blank card.
- **`wait_basis`** names the estimator, so the page says "Typical wait is estimated as
  half the average gap between buses" instead of asserting a method the firmware may
  have changed. Unknown values are de-underscored and shown as-is.

Counts are read as `data.samples ?? data.arrivals` and `data.late_known ??
data.late_known_n`, so a rename on the firmware side does not blank the page.
`coverage` is accepted as either a 0-1 fraction or a 0-100 percentage.

### Setting hints

Every control on Settings, and the Indego card and stop edit form on Stops, has a
`<p class="hint">` under it in plain language — what the setting does, and what changes
if you flip it. They exist because the owner is not a programmer and an unexplained
setting is one nobody dares touch. The shared ones (rows to show, title style,
alternative-of, rail line, bus/trolley) live in `HINT_*` constants near the top of
`app.js` so the add wizard and the edit form cannot drift apart. The CSS selector is
`p.hint`, deliberately not `.hint`, so it does not also catch `footer.hint`. On Settings
the hint travels inside the same `field()` wrapper as its control, which is what lets the
filter box match on the plain-language text as well as the label.

## Assumptions made (DESIGN.md §7/§9.3 didn't fully pin these down)

These are mock-server/UI implementation choices, not firmware behavior — flag them
if the actual device ends up shaped differently:

- **Firmware version in `/api/state`**: §7 lists `time, uptime, heap, wifi{}, sd{},
  last_poll{}, stops[], alerts[]` for `/api/state` but the Settings view also needs a
  firmware version string per §10. Added `firmware_version` as an extra top-level
  field on `/api/state` (mock returns `"0.1.0-mock"`).
- **`by_weekday` bin key**: §9.3 shows `by_hour` entries as `{h, n, mean, p50, p90}`
  but only writes `"by_weekday": [ ... ]` without naming the day field. The mock uses
  `d` (0=Sunday, matching `Date.getDay()`); the client reads `d ?? weekday ?? day ?? wd`
  defensively so it still renders if the real firmware names it differently.
- **`GET /api/stats/overview`, `crowding`, and `wait_by_hour`**: none of these are in
  DESIGN.md §9.3 yet — they're new per this round of work. Shapes used here: overview
  rows are `{stop, samples, on_time_pct, mean_late_min, ghost, noshow, last_seen_ts}`
  plus a top-level `bikes: [{station, name, by_hour}]`; `crowding` is `{by_hour,
  by_weekday}` of `{h|wd, n, mean, dist[6]}` (mean and per-sample counts over the same
  0-5 levels as `CROWD_WORDS` in `app.js`); `wait_by_hour` is `{h, n, mean_gap_s,
  max_gap_s, ghost, noshow}`. Flag it if the firmware lands a different shape.
- **`headway.ratio_hist` shape**: not specified beyond "a histogram of the ratio".
  The mock emits `[{ pct_lo, pct_hi, bucket, count }, ...]` over 5 bins (0-40,
  40-80, 80-120, 120-175, 175+, matching the bunched/gapped thresholds in §9.2). The
  client (`normalizeHist` in `app.js`) accepts that shape, `{count|n, bucket|label}`,
  or a plain array of numbers, so it won't break against a differently-shaped real
  response — only the per-bucket color coding depends on `pct_lo`/`pct_hi` being
  present.
- **No per-route color config**: §6's config schema has no `color` field, so route
  badges always use the theme's accent color rather than a per-route color.
- **Subway stop entry**: §4.6 says subway (`BSL`/`MFL`) stops don't resolve through
  `/api/proxy/stops`. When that lookup returns `[]` for `mode: subway`, the wizard
  falls back to a manual stop_id + name entry, then still calls
  `/api/proxy/schedule?stop_id=` to see if it resolves; if that also comes back
  empty, the wizard shows "not supported yet" per §4.6's requirement rather than
  failing silently.
- **`/api/proxy/schedule` for stop_ids without a fixture**: the real SEPTA endpoint
  takes only `stop_id` (no route), so a stop_id the mock doesn't recognize is
  synthesized under route "17" (the only populated `stops_*.json` fixture) with two
  fabricated directions, so the Add Stop wizard is exercisable for any of the 133
  route-17 stops, not just the two canned ones.
- **Rail station list shape**: `/api/rail/stations` is documented only as "static
  Regional Rail station list." The mock (and client) treat it as an array of either
  plain strings or `{name}` objects.
- **`device.ticker_show`**: not in DESIGN.md §6 — added so the owner can pick what the
  alert ticker displays (`both` (default), `alerts`, `detours`, `off`) independently of
  the existing `alerts` flag, which controls whether alerts are fetched at all. Mock
  validates it as one of those four strings, `400`-ing with
  `device.ticker_show` otherwise.

## Verifying changes

```
node --check web/app.js && node --check web/mock-server.mjs && node --check web/build.mjs
node web/mock-server.mjs 8080 &                              # run it
node web/build.mjs && node web/build.mjs --check              # rebuild + freshness check
```

There's no headless-browser check in this repo; exercise the UI by hand against the
mock server, or against real hardware once the firmware side serves the generated
header.
