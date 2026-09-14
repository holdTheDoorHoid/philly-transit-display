# SEPTA / Philadelphia-region data sources for an ESP32 desk display

Researched live on 2026-09-13. All endpoints below were actually fetched with `curl`
(not just documentation-read) unless explicitly marked **UNVERIFIED**. Byte sizes are
real `curl -w '%{size_download}'` measurements from this session and will vary run to
run (more/fewer active vehicles, more/fewer trips left in the day), but the orders of
magnitude are stable.

Device constraints assumed throughout: ~320 KB RAM, no PSRAM, one TLS connection costs
~40-50 KB, comfortable response-buffering ceiling ~30-60 KB.

---

## 1. SEPTA API v1 (`www3.septa.org/api/`)

The old marketing/docs page at `http://www3.septa.org/hackathon/` is now dead
(**403 Forbidden**, both http and https, no index). The API itself is alive and is
**still called "v1" only informally** — there is no version number in the URLs or in
the spec (see §4). The current canonical docs are a Swagger/OpenAPI spec served from
`https://app.septa.org/apidoc.json` (also mirrored, byte-identical UI, at
`https://api.septa.org/`), title "SEPTA Developers", spec version `1.0.2`. No
`securityDefinitions` are declared anywhere in the spec — **confirmed: no API key,
anywhere, for any v1 endpoint.**

All endpoints are plain HTTP(S) `GET` with query-string params, returning JSON (except
where noted). **HTTPS cert chain**: `www3.septa.org` presents `CN=*.septa.org` issued by
`Amazon RSA 2048 M01` (Amazon Trust Services), valid 2026‑09‑09 → 2027‑03‑25 — a
standard public CA. Any ESP32 TLS stack using the Mozilla/ESP-IDF default cert bundle
(which includes the Amazon Trust roots) will validate it with no custom pinning needed.

No documented rate limit exists anywhere in the spec or the license page. Treat that as
"undocumented," not "unlimited" — poll conservatively (see §8).

### BusSchedules — `GET /api/BusSchedules/index.php`
- **Official param**: `stop_id` (integer). The legacy `req1` alias also works in
  practice but isn't in the current spec.
- **What it returns**: the *scheduled* (GTFS-timetable) departure times for a stop,
  **not real-time predictions**. There is no lateness/delay field in this response —
  to compute lateness you must cross-reference with TransitView or GTFS-RT TripUpdates.
- Verified sample (stop 21297, "20th St & Mifflin St", route 17), **722 bytes**, HTTP 200:
  ```json
  {"17":[
    {"StopName":"20th St & Mifflin St","Route":"17","trip_id":"281678","date":"10:32p","day":"Sun","Direction":"0","DateCalender":"09/13/26 10:32 pm","DirectionDesc":"2nd-Market"},
    {"StopName":"20th St & Mifflin St","Route":"17","trip_id":"281677","date":"11:01p","day":"Sun","Direction":"0","DateCalender":"09/13/26 11:01 pm","DirectionDesc":"2nd-Market"},
    {"StopName":"20th St & Mifflin St","Route":"17","trip_id":"281692","date":"11:31p","day":"Sun","Direction":"0","DateCalender":"09/13/26 11:31 pm","DirectionDesc":"2nd-Market"},
    {"StopName":"20th St & Mifflin St","Route":"17","trip_id":"281674","date":"12:03a","day":"Mon","Direction":"0","DateCalender":"09/14/26 12:03 am","DirectionDesc":"Front-Market"}
  ]}
  ```
- **Reliability quirk observed**: this endpoint intermittently returns HTTP **400** or
  even HTTP **501** for a *valid* stop_id that succeeds moments later on retry (see
  §5 for a reproduced example on stop 21332). Response headers show requests are load
  balanced across multiple backend nodes (`X-B-Srvr: api_main1` vs `api_main5` behind
  an AWS ELB) — the flakiness looks like one backend node occasionally serving a stale
  error. **A client must retry on error rather than treat 400/501 as terminal.**
- No API key. Response size: consistently a few hundred bytes to ~1 KB per stop —
  trivially small.

### TransitView — `GET /api/TransitView/index.php?route=<route>`
- Real-time bus/trolley vehicle positions for one route. Includes a `late` field
  (minutes late, can be negative = early) **computed by SEPTA server-side** — this is
  the actual predicted-vs-scheduled delta, no client-side math needed.
- Verified sample (`route=17`), **1538 bytes**, HTTP 200:
  ```json
  {"bus":[
    {"lat":"39.913612","lng":"-75.181739","label":"7481","route_id":"17","trip":"3654",
     "VehicleID":"7481","BlockID":"7309","Direction":"Southbound","destination":"20th-Johnston",
     "heading":284.77,"late":2,"next_stop_id":"31456","next_stop_name":"Moyamensing Av & 20th St",
     "next_stop_sequence":46,"estimated_seat_availability":"FEW_SEATS_AVAILABLE",
     "Offset":2,"Offset_sec":"90","timestamp":1789351486},
    ... 2 more vehicles ...
  ]}
  ```
- `Direction` here is the compass form (`Northbound`/`Southbound`); `destination` is
  the named-terminus form (`20th-Johnston`, `2nd-Market`) — see §5 for how these two
  naming schemes map to each other.
- `TransitViewAll` (all routes system-wide, no route filter) also exists and works,
  but is **129,045 bytes** — far too big to buffer on this device. Always use the
  per-route form.
- No API key. Per-route size: 1-3 KB typically (scales with vehicles currently on that
  route) — small and practical.

### Alerts — `GET /api/Alerts/index.php`
- **Official param is `routes`**, not a bare route number, and it needs a mode
  prefix: `bus_route_17`, `trolley_route_10`, `rr_route_chw`, etc. (Get the exact
  list from `https://www3.septa.org/api/Alerts`.) Passing a bare `req1=17` is
  silently accepted and returns `[]` (2 bytes) — **it looks like "no alerts" but is
  actually "wrong parameter,"** a real footgun for anyone copying the old `req1`
  convention from other endpoints.
- Verified sample (`routes=bus_route_17`), **1813 bytes**, HTTP 200 — includes
  service-change advisory HTML, an array of active `detour` entries (each with
  location, start/end datetime, and free-text reason), and an `elevator` array (empty
  here). No numeric delay/schedule data — this is alert/detour text only.
- There is also `Alerts/get_alert_data.php`, tested with a bad param and got a
  56-byte `{"Error":true,...}` — didn't find its correct valid parameter in this
  session; **UNVERIFIED** for actual use, `Alerts/index.php?routes=` covers the need.
- No API key. Size: 1-3 KB per route when there's content — fine.

### Arrivals (Regional Rail) — `GET /api/Arrivals/index.php`
- Params: `station` (required, station name string), `results` (optional count),
  `direction` (optional `N`/`S`).
- **This is the one v1 endpoint that gives you both a schedule time and a live
  prediction in the same record**: `sched_time` and `depart_time` are separate
  fields, plus a human `status` string (`"On Time"`, `"1 min"`, etc.) that's
  effectively the delay already computed for you.
- Verified sample (`station=30th Street Station`), **3456 bytes**, HTTP 200:
  ```json
  {"Gray 30th Street Departures: September 13, 2026, 10:05 pm":[
    {"Northbound":[
      {"direction":"N","path":"R5/8N","train_id":"5878","origin":"Wynnewood","destination":"Fox Chase",
       "line":"Paoli/Thorndale","status":"1 min","service_type":"LOCAL","next_station":"Wynnewood",
       "sched_time":"2026-09-13 22:24:00.000","depart_time":"2026-09-13 22:25:00.000",
       "track":"1","track_change":null,"platform":"","platform_change":null},
      ...
    ]},
    {"Southbound":[ ... ]}
  ]}
  ```
- Per the spec's own description: "Northbound"/"Southbound" here are **not compass
  directions** — they're legacy references to the old Reading (Northbound) vs.
  Pennsylvania (Southbound) railroad divisions that SEPTA's Regional Rail system
  was built from. Don't assume geography from this field for rail.
- No API key. Size: a few KB per station — practical, and this is probably the best
  single endpoint for a rail-stop widget.

### Stops — `GET /api/Stops/index.php?req1=<route>`
- Returns every stop on a route (id, name, lat/lng) — a route-wide reference list,
  not a live feed. Verified `req1=17`: **133 stops, 12,043 bytes**, HTTP 200.
- Useful once, at setup time, to resolve a human-picked stop to a `stop_id` — not
  something to re-poll. No API key.

### TrainView — `GET /api/TrainView/index.php`
- All active Regional Rail trains system-wide (no per-line filter parameter exists).
  Verified: **4711 bytes**, ~dozens of trains, includes `late` (minutes), `consist`
  (car numbers), `line`, `dest`, `currentstop`/`nextstop`. No API key. Small enough
  to buffer, but you get the whole system, not just your line — client-side filter
  by `line`/`dest`.

### Other v1 endpoints found in the Swagger spec (bonus, not in the original ask)
Discovered via `apidoc.json`'s full path list — all confirmed live:
- `NextToArrive/index.php?req1=<origin>&req2=<dest>&req3=<n>` — direct
  station-to-station next-departure lookup. Verified 30th St → Trenton: **295 bytes**.
  Very compact; good alternative to `Arrivals` if the user only cares about one
  destination.
- `BusDetours/index.php?req1=<route>` — verified route 17: **1289 bytes**.
- `elevator/index.php` — system-wide elevator/escalator outage list (subway/EL, not
  bus). Verified: **2987 bytes**, 9 outages at fetch time, updated timestamp included.
  This answers part of §6/§7: **yes, SEPTA does publish an elevator-outage feed**, but
  it's subway/EL stations, not bus stops.
- `RRSchedules/index.php?req1=<train#>`, `sms/index.php`,
  `locations/get_locations.php` — present in the spec, not exercised in this session.

---

## 2. SEPTA GTFS-Realtime feeds

Base path: `https://www3.septa.org/gtfsrt/`. Two feed families —
`septa-pa-us` (bus/trolley) and `septarail-pa-us` (Regional Rail) — each with
`Trip`, `Vehicle`, and `Service` sub-paths. **The exact filenames are irregular and
not discoverable by pattern-guessing** (directory listing is 403'd); confirmed
working filenames:

| Feed | URL | Size (measured) | Entities |
|---|---|---|---|
| Bus trip updates | `.../gtfsrt/septa-pa-us/Trip/rtTripUpdates.pb` | **~157 KB** | (many) |
| Bus vehicle positions | `.../gtfsrt/septa-pa-us/Vehicle/rtVehiclePosition.pb` (singular "Position") | **~13.4 KB** | 185 |
| Bus service alerts | `.../gtfsrt/septa-pa-us/Service/rtServiceAlerts.pb` | **~59 KB** | 285 |
| Rail trip updates | `.../gtfsrt/septarail-pa-us/Trip/rtTripUpdates.pb` | **~2.9-3.1 KB** | 29 |
| Rail vehicle positions | `.../gtfsrt/septarail-pa-us/Vehicle/rtVehiclePosition.pb` | **~3.4 KB** | (small) |
| Rail service alerts | `.../gtfsrt/septarail-pa-us/Service/rtServiceAlerts.pb` | **~4.3 KB** | (small) |

All confirmed with a real GTFS-RT protobuf parser (`gtfs-realtime-bindings` for
Python) — these are genuine `FeedMessage`, `gtfs_realtime_version: "2.0"`, not error
pages. Sample decoded rail trip-update entity:
```
id: "LAN3570_20260621_SID186629"
trip_update {
  trip { trip_id: "LAN3570_20260621_SID186629" schedule_relationship: SCHEDULED route_id: "LAN" direction_id: 0 }
  stop_time_update { stop_sequence: 0 arrival { delay: 60 uncertainty: 60 } }
  vehicle { id: "146,145,414" }
}
```
Sample decoded bus vehicle-position entity:
```
id: "3456"
vehicle {
  trip { trip_id: "937453" route_id: "103" direction_id: 0 }
  position { latitude: 39.9620247 longitude: -75.2580261 bearing: 260.99 }
  current_stop_sequence: 1
  timestamp: 1789351630
  stop_id: "33105"
}
```
There is also a human-readable `print.php` on each sub-path (e.g.
`.../Trip/print.php`) that dumps only the most recent 5 records as HTML — not useful
for a device, only for debugging in a browser.

**Cadence**: not documented; these are pushed from SEPTA's AVL system, effectively
near-real-time (seconds-old timestamps observed). No published minimum poll interval.

**Practicality on a 320 KB / no-PSRAM device — be honest:**
- **Bus TripUpdates (~157 KB) and Bus ServiceAlerts (~59 KB): not practical.** The
  bus trip-updates feed alone exceeds the device's entire RAM before you even add a
  TLS session, WiFi stack, and display buffers. Even the alerts feed, at ~59 KB, sits
  right at the edge of the stated 30-60 KB buffering ceiling — fitting it would
  require a genuinely streaming protobuf parser (impossible to just
  `HTTPClient.getString()` into a buffer), and 320 KB total RAM leaves very little
  headroom once you subtract ~40-50 KB for the TLS session itself. This is not
  something to build without a fairly serious embedded protobuf effort (e.g. a
  hand-rolled streaming varint/length-delimited-field walker over the wire as bytes
  arrive — `nanopb` can do this but you'd still need to write the streaming callback
  logic, it doesn't come for free).
- **Bus VehiclePosition (~13.4 KB) and all three rail feeds (2.9-4.3 KB): size-wise
  practical** even with full-buffer-then-parse, and small enough that even a naive
  nanopb one-shot decode is plausible RAM-wise. The catch is protobuf parsing is
  still strictly more device-side work than the JSON v1 API for the exact same
  underlying data — **for a single-purpose desk display, the v1 JSON API in §1 is a
  better fit than GTFS-RT** unless you specifically need the standardized
  GTFS-RT schema (e.g. to reuse code across cities, see §7).

---

## 3. SEPTA GTFS static

- **Outer bundle**: `http://www3.septa.org/developer/download.php` — **21,675,909
  bytes (~20.7 MB)**, `Content-Type: application/zip`, no license click-through
  needed to fetch it directly (the click-through page at
  `https://www3.septa.org/developer/` is just a human landing page; the file itself
  has no auth). Confirmed it's a zip-of-zips:
  ```
  Archive:  download.zip
    Length      Date    Time    Name
  20918311  2026-09-09 15:49   google_bus.zip
    757262  2026-09-09 15:49   google_rail.zip
  ```
- Each inner zip is also fetchable directly and matches: `google_bus.zip` at
  `https://www3.septa.org/developer/google_bus.zip` = 20,918,311 bytes;
  `google_rail.zip` at `.../developer/google_rail.zip` = 757,262 bytes.
- The developer landing page reports **"GTFS updated: Fri, 11 Sep 2026 16:14:24"**
  (2 days before this research) — this feed is actively maintained, not stale.
- **Structure (bus)**: standard GTFS plus some GTFS-Fares-v2 and pathways extensions —
  `agency, calendar, calendar_dates, directions, fare_leg_join_rules, fare_leg_rules,
  fare_media, fare_products, fare_transfer_rules, feed_info, levels, pathways,
  rider_categories, route_stops, routes, shapes, stops, stop_times, trips`.
  Uncompressed, `stop_times.txt` alone is **102 MB** and `shapes.txt` is 18.8 MB —
  the full static feed is obviously never something to put on the device; at most a
  server-side proxy would ingest it.
- **Stop ID format**: plain small integers as strings, e.g. `21297`, `21332`, `17`
  (route). `stops.txt` for route 17's stops:
  ```
  stop_id,stop_code,stop_name,stop_lat,stop_lon,zone_id,stop_url,location_type,parent_station,wheelchair_boarding,level_id
  21332,21332,19th St & Mifflin St,39.927947,-75.177147,1,,0,,1,
  21297,21297,20th St & Mifflin St,39.927942,-75.178646,1,,0,,1,
  ```
- **Mapping to the v1 BusSchedules `stop_id`**: **confirmed identical** — `stop_id`
  21332/21297/etc. in `stops.txt` are the exact same numbers accepted by
  `BusSchedules/index.php?stop_id=`, and `stop_code` equals `stop_id` too (no separate
  public-facing code). Route 17 in `routes.txt`:
  `17,1,17,Front-Mkt to 20-Johnston,3,https://www.septa.org/schedules/17,EF3340,FFFFFF,20170,bus_metro`
  — the `route_long_name` ("Front-Mkt to 20-Johnston") matches the `DirectionDesc`
  strings seen in the live API (§5). **No ID translation layer needed** between GTFS
  static and the v1 JSON API — this materially simplifies the device/proxy design.

---

## 4. Is there a v2 API / developer portal with keys?

**No.** There is a modern-looking developer portal (Swagger UI) at both
`https://app.septa.org/` and `https://api.septa.org/` (same content, both served from
a static Swagger-UI bundle pointing at `apidoc.json`), and this **is** the current
official documentation for the endpoints in §1 — but:
- Spec `info.version` is `1.0.2` — still "v1" in substance, just documented properly.
- `securityDefinitions` is absent/`null` — **no API key, no OAuth, no registration
  step of any kind** for any endpoint in the spec.
- There is no `api.septa.org` REST surface distinct from `www3.septa.org/api/` — it's
  the same backend (`basePath: /api`), just documented on a different domain.

So: the "hackathon" era API and the current API are the same API. SEPTA has not
introduced a versioned, key-gated v2. (I did not find any SEPTA blog post, press
release, or GitHub issue announcing deprecation of the v1 endpoints or a migration
timeline — if one exists it isn't linked from the developer portal or the Swagger
spec, so treat "no known deprecation" as the current state, not a guarantee.)

---

## 5. Route 17 specifics

Route 17 runs on the 19th/20th Street couplet through South Philadelphia (and
continues north through Center City/Old City). GTFS `route_long_name`: **"Front-Mkt
to 20-Johnston"**. Two direction-naming conventions coexist across endpoints:

| Endpoint | Compass form | Named-terminus form |
|---|---|---|
| TransitView | `Direction: "Northbound"` / `"Southbound"` | `destination: "2nd-Market"` / `"20th-Johnston"` |
| BusSchedules | `Direction: "0"` / `"1"` | `DirectionDesc: "2nd-Market"`/`"Front-Market"` (0) / `"20th-Johnston"` (1) |

So: **Northbound = Direction 0 = toward 2nd-Market/Front-Market** (Old City);
**Southbound = Direction 1 = toward 20th-Johnston** (South Philly/Packer Park area,
though per the live Alerts response the route was recently shortened — see below).

**Stops at/near 19th & Mifflin** (pulled from `Stops/index.php?req1=17`, 133 stops
total on the route):

| Stop ID | Name | Direction served | Notes |
|---|---|---|---|
| **21332** | 19th St & Mifflin St | Southbound (Direction 1, "20th-Johnston") | Exact corner. See reliability note below. |
| **21297** | 20th St & Mifflin St | Northbound (Direction 0, "2nd-Market"/"Front-Market") | Exact corner, opposite direction. |
| 21331 | 19th St & Moore St | Southbound | One block north of 21332, same direction — useful fallback. |
| 21298 | 20th St & Moore St | (not queried) | One block north of 21297. |

Live `BusSchedules` responses for both exact-corner stops, fetched 2026-09-13 ~22:07
local time:

```json
// stop_id=21297 (20th & Mifflin, Northbound)
{"17":[
  {"StopName":"20th St & Mifflin St","Route":"17","trip_id":"281678","date":"10:32p","day":"Sun","Direction":"0","DateCalender":"09/13/26 10:32 pm","DirectionDesc":"2nd-Market"},
  {"StopName":"20th St & Mifflin St","Route":"17","trip_id":"281677","date":"11:01p","day":"Sun","Direction":"0","DateCalender":"09/13/26 11:01 pm","DirectionDesc":"2nd-Market"},
  {"StopName":"20th St & Mifflin St","Route":"17","trip_id":"281692","date":"11:31p","day":"Sun","Direction":"0","DateCalender":"09/13/26 11:31 pm","DirectionDesc":"2nd-Market"},
  {"StopName":"20th St & Mifflin St","Route":"17","trip_id":"281674","date":"12:03a","day":"Mon","Direction":"0","DateCalender":"09/14/26 12:03 am","DirectionDesc":"Front-Market"}
]}
```

```json
// stop_id=21332 (19th & Mifflin, Southbound)
{"17":[
  {"StopName":"19th St & Mifflin St","Route":"17","trip_id":"281757","date":"10:25p","day":"Sun","Direction":"1","DateCalender":"09/13/26 10:25 pm","DirectionDesc":"20th-Johnston"},
  {"StopName":"19th St & Mifflin St","Route":"17","trip_id":"281756","date":"10:52p","day":"Sun","Direction":"1","DateCalender":"09/13/26 10:52 pm","DirectionDesc":"20th-Johnston"},
  {"StopName":"19th St & Mifflin St","Route":"17","trip_id":"281755","date":"11:21p","day":"Sun","Direction":"1","DateCalender":"09/13/26 11:21 pm","DirectionDesc":"20th-Johnston"},
  {"StopName":"19th St & Mifflin St","Route":"17","trip_id":"281754","date":"11:49p","day":"Sun","Direction":"1","DateCalender":"09/13/26 11:49 pm","DirectionDesc":"20th-Johnston"}
]}
```

**Reliability note (real, reproduced, worth flagging):** the very first request for
`stop_id=21332` in this session returned **HTTP 400** — `{"error": "An invalid
parameter was used. Ensure 'req1' or 'stop_id' is assigned a valid stop id."}` —
for a stop ID that is unambiguously valid (present in `Stops` and in GTFS static). Two
immediate retries both succeeded with HTTP 200 and the data above. This looks like
transient backend flakiness (see the load-balancer note in §1), not a real
data/mapping problem with that stop. **Recommendation for the device: on any
BusSchedules error response, retry once or twice before surfacing an error to the
user** — don't treat a single 400 as "this stop doesn't exist."

**Live route-17 alert** (fetched via `Alerts/index.php?routes=bus_route_17`, relevant
to anyone actually building against this route right now): SEPTA's "New Bus Network"
restructuring has **shortened Route 17** — it "no longer serve[s] Packer Park, [B] NRG
or the Navy Yard," with Route 45 covering the Navy Yard instead, plus active
construction detours near 5th & Market and 2nd & Market through the end of 2026, and
a pothole-related NB detour near 20th & Washington through 2026-09-14. This is exactly
the kind of thing a lateness/prediction feature would otherwise not explain — worth
surfacing detour/alert text alongside predictions if it's cheap to do (it's ~1-2 KB,
so it is).

---

## 6. Other regional data sources

| Source | URL | Auth | Size (measured) | Notes |
|---|---|---|---|---|
| **PATCO** (Speedline, NJ↔Philadelphia) GTFS static | `https://rapid.nationalrtap.org/GTFSFileManagement/UserUploadFiles/13562/PATCO_GTFS.zip` | none | **44,501 bytes** | Confirmed live, `feed_version 17`, `feed_start_date 20260807`, `feed_end_date 20261231` — currently valid. (A third-party aggregator's cached record claiming PATCO's feed "expired Jan 1 2026" refers to a stale snapshot, not the live feed — the file fetched today is current.) Official page: `https://www.ridepatco.org/developers/`. |
| **PATCO GTFS-Realtime** | — | — | — | **UNVERIFIED / likely does not exist.** No realtime section on PATCO's developer page, and no working feed URL found by search. |
| **NJ Transit** bus + rail GTFS / GTFS-RT | `https://developer.njtransit.com/registration/` | **Requires developer account + API key registration** | not fetched | Portal migrated from the old `njtransit.com/developer-tools` (retired Aug 2024). Registration and key issuance is an account-creation step — **not something I can or should do on your behalf**; you'd need to sign up yourself to get a key, after which trip-update/vehicle-position/GTFS endpoints are documented to exist for both bus and rail. |
| **Amtrak** (30th St Station) | GTFS static: `https://content.amtrak.com/content/gtfs/GTFS.zip` (per Mobility Database listing, **not independently fetched this session**) | none for static | UNVERIFIED size | **No official real-time API.** Amtrak.io's "Live Train Status API" page says the API is "coming soon." Real-time Amtrak position data in the wild comes from reverse-engineered/community GTFS-RT projects (e.g. `catenarytransit/amtrak-gtfs-rt`, `kurtraschke/amtk-gtfsrealtime` on GitHub) built from Amtrak's public train-status map, not an Amtrak-published feed — treat as unofficial/fragile if used. |
| **Indego** bike share (GBFS) | Auto-discovery: `https://gbfs.bcycle.com/bcycle_indego/gbfs.json` | none | **730 bytes** (discovery doc) | The commonly-cited `gbfs.rideindego.com/gbfs.json` did not resolve/connect in this session (DNS/connection failure) — use the `bcycle.com` host, which is Indego's actual GBFS backend (Indego is operated on the BCycle platform) and did resolve and return valid GBFS 1.1 JSON. Sub-feeds: `station_information.json` (**128,868 bytes**, static-ish, all ~230 stations), `station_status.json` (**72,153 bytes**, live availability, poll this one), `system_information.json` (495 bytes). No per-station query — you fetch the whole city and filter client-side, which is itself a reason a small proxy is useful (§8). |
| **SEPTA elevator outages** | `https://www3.septa.org/api/elevator/index.php` | none | **2987 bytes** | Confirmed in §1 — subway/EL elevators only, not bus stops. |
| **SEPTA alerts** | `https://www3.septa.org/api/Alerts/index.php?routes=<mode>_route_<id>` | none | 1-3 KB/route | Confirmed in §1. |

---

## 7. Generic portability (kept short)

- **Mobility Database** (`mobilitydatabase.org`, successor to the old
  TransitFeeds/OpenMobilityData catalog, backed by MobilityData) is the right
  starting point for porting this device to another city: it catalogs GTFS and
  GTFS-RT feeds for essentially every US transit agency with machine-readable feed
  URLs, licensing, and last-verified dates, so a port doesn't require re-doing this
  kind of manual endpoint archaeology from scratch. I did not exhaustively re-verify
  its catalog contents this session — treat it as a starting index to check, not a
  guarantee every listed URL still works (as this whole report demonstrates, transit
  agency URLs rot and get renamed).
- **Agencies with a small, purpose-built per-stop/per-route JSON prediction API**
  (the SEPTA-v1 style, easy on a memory-constrained device) — from general knowledge,
  **not independently re-verified in this session**, so treat as a starting list to
  confirm before relying on: **MBTA** (Boston) has a well-documented, actively
  maintained `api-v3.mbta.com` with per-stop `/predictions` JSON and API-key-gated
  higher rate limits (free tier exists); **CTA** (Chicago) Bus Tracker and Train
  Tracker both have small XML/JSON per-stop/per-route endpoints with a free API key;
  **MTA** (NYC) publishes GTFS-RT for subway and has SIRI-based per-stop bus
  predictions (BusTime); **WMATA** (DC) has a REST API with per-stop
  `NextBusService`/`NextTrain` JSON, API-key gated. All four of these are more
  formally documented and rate-limited than SEPTA's key-less v1 API.
- **The more common case industry-wide is GTFS-RT only**, with no small JSON
  alternative — meaning a device built to be portable across many US cities should
  assume it may need actual streaming protobuf parsing capability eventually, even
  though SEPTA specifically lets you avoid it today via the v1 JSON API.

---

## 8. Recommendations

**What to poll for per-stop predictions today (SEPTA, this device):**
- **Bus/trolley stops**: `TransitView/index.php?route=<route>` (1-3 KB, has
  server-computed `late` minutes, next-stop info) as the primary live signal, plus a
  one-time `Stops`/`BusSchedules` lookup to know which `stop_id`/direction on that
  route corresponds to the user's chosen physical stop. `BusSchedules` alone
  (scheduled-only, no live delay) is a reasonable fallback when TransitView shows no
  vehicle currently tracked for that trip.
- **Regional Rail stops**: `Arrivals/index.php?station=<name>` (a few KB, already
  includes both `sched_time` and `depart_time`/`status` — no separate schedule
  lookup needed). Use `NextToArrive` instead if the display only ever needs "next
  train to specific destination X," since it's smaller (~300 bytes) and simpler to
  parse.
- **Skip GTFS-RT entirely for this device.** Per §2, the JSON equivalents are both
  smaller and require zero protobuf tooling; GTFS-RT only becomes worth it if this
  code needs to be portable to agencies that don't offer a JSON API (§7), which
  SEPTA does.
- **Poll cadence**: SEPTA publishes no documented rate limit, which cuts both ways —
  there's no quota to respect, but also no SLA. Given TLS setup cost (~40-50 KB and
  real time/battery cost) dominates over the tiny response bodies, batch: do one TLS
  session, fetch the 1-2 endpoints you need back to back on the same connection if
  your HTTP client supports keep-alive, then close it. A **30-60 second poll
  interval** is a reasonable default for a desk display (predictions for a bus a few
  minutes out don't need sub-minute freshness), tightened to ~15-20s only if a
  vehicle is imminent. Always implement retry-with-backoff given the observed
  transient 400/501 responses (§1, §5) rather than surfacing a hard error to the user
  on the first failure.
- **What a lightweight optional proxy/cache would need to do, if added later:**
  1. Hold the TLS connection to SEPTA (and any other agency) server-side, so the
     device only ever does plaintext HTTP (or a much cheaper TLS session) to a LAN or
     low-latency proxy — removes the 40-50 KB TLS tax from the device entirely.
  2. Pre-filter large, un-scoped feeds the device should never see whole: GBFS
     `station_status.json` (72 KB → the 1-2 stations the user cares about, a few
     hundred bytes), `TransitViewAll` (129 KB → one route), and, if GTFS-RT is ever
     wanted, decode the protobuf once server-side and re-emit tiny per-stop JSON.
  3. Cache the GTFS static zip (20+ MB) and do the stop_id ⇄ route ⇄ direction
     resolution server-side, so the device only ever needs to know a small integer
     `stop_id` it was configured with — it never has to fetch or parse GTFS static
     itself.
  4. Absorb the retry/backoff logic and normalize the two direction-naming schemes
     (§5) into one consistent shape before handing JSON to the device.
  5. Optionally coalesce multiple downstream sources (SEPTA + PATCO + Indego) behind
     one small per-device JSON contract, which is also what makes the "generic
     portability" goal in §7 realistic — the device's parser never needs to change
     per city, only the proxy's backend does.

---

## Flags / things I could not fully verify

- `Alerts/get_alert_data.php`'s correct parameter — only saw its error response, not
  a working call.
- PATCO GTFS-Realtime existence — treated as likely-absent based on the developer
  page's contents and search, not a definitive negative.
- NJ Transit live endpoint behavior, rate limits, and exact URL paths — gated behind
  developer registration I did not create an account for.
- Amtrak GTFS static exact byte size — cited from the Mobility Database listing, not
  independently downloaded/measured this session.
- Mobility Database's current catalog contents and the MBTA/CTA/MTA/WMATA endpoint
  details in §7 — from general knowledge, not re-fetched/re-verified this session;
  confirm live before building against them.
- No explicit SEPTA deprecation notice was found for the v1 API, but absence of a
  notice on the pages I checked isn't proof one doesn't exist elsewhere (e.g. a
  mailing list post to the `SEPTAdev` Google Group, which I did not read).
