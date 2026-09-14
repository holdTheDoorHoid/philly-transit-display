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

Then open `http://localhost:8080/`. The mock server implements every route in
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
  in the Now view). One alert (from `alerts_bus_17.json`) is included with its HTML
  advisory text stripped to plain text.
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
- `GET /api/stats?stop=&days=` returns deterministic synthetic aggregates in the
  exact §9.3 shape (same `stop`+`days` always produces the same numbers). Add
  `&empty=1` to get the zero-sample "no data yet" shape, useful for testing that UI
  state.
- `GET /api/log/index` / `GET /api/log/<file>` — a synthetic two-file index and CSV
  content using the exact §9.1 header and column order.
- `POST /api/ota` drains the multipart body and replies after a short simulated
  delay; `POST /api/reboot` and `POST /api/wifi/reset` reply `200` immediately.

\* `node:zlib` is only used by `build.mjs`; the mock server itself doesn't gzip
anything (see above).

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

| Asset | Raw | Gzip |
|---|---:|---:|
| `index.html` | 848 B | 433 B |
| `app.js` | ~77 KB | ~20.2 KB |
| `app.css` | ~9.9 KB | ~2.8 KB |
| `favicon.svg` | 410 B | 202 B |
| **Total** | **~88 KB** | **~23.6 KB** |

That leaves roughly 36 KB of headroom under the budget. `build.mjs` prints a warning
(without failing) if the total ever exceeds 60,000 bytes gzip.

## How the UI maps to the device API

| View | Reads | Writes |
|---|---|---|
| **Now** | `GET /api/state` every 15 s | — |
| **Stops** | `GET /api/config`, `GET /api/proxy/stops`, `GET /api/proxy/schedule`, `GET /api/rail/stations` | `PUT /api/config` (reorder, edit, remove, add — always sends the whole config) |
| **Stats** | `GET /api/config` (stop picker), `GET /api/stats`, `GET /api/log/index` | — (CSV download links point at `GET /api/log/<file>`) |
| **Settings** | `GET /api/config`, `GET /api/state` (firmware version) | `PUT /api/config`, `POST /api/ota`, `POST /api/reboot`, `POST /api/wifi/reset` |

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

Each configured stop's edit form (§6 "Fields added 2026-09-14") also sets how its
title is shown on screen (`title_style`: label → destination, label only, route →
destination • stop, or custom text saved as `title_text`) and whether the stop is an
*alternative* — hidden unless another configured stop's next arrival is more than
`alt_after_min` minutes away (`alt_of`, the other stop's `key`). The stop list preview
computes the on-screen title client-side the same way the firmware will, and shows an
"alternative to …" hint when `alt_of` is set.

The Settings view additionally covers, in order after the alert ticker section: large
text / crowding display extras, quiet hours (backlight dims on a schedule, touch wakes
it), the night clock, "time to leave" LED/screen/chime alerts, up to 4 schedule-based
**profiles** (each with a name, days, a time window, and its own ordered stop list —
checkboxes plus ↑/↓ reordering, built from the configured stops), and Indego bike
stations (§4.9): a manual station-id add, and a "Find stations near my stops" lookup
that fetches the Bicycle Transit status feed directly in the browser and ranks the six
nearest stations by great-circle distance from stops that have `lat`/`lng`. That fetch
is plain HTTP; if the web UI itself is loaded over HTTPS the browser blocks it as mixed
content and the button shows a banner explaining that instead of failing silently.

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

## Verifying changes

```
node --check web/app.js && node --check web/mock-server.mjs && node --check web/build.mjs
node web/mock-server.mjs 8080 &                              # run it
node web/build.mjs && node web/build.mjs --check              # rebuild + freshness check
```

There's no headless-browser check in this repo; exercise the UI by hand against the
mock server, or against real hardware once the firmware side serves the generated
header.
