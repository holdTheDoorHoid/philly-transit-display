# transit_core verification notes

Everything below was fetched live on 2026-09-13 (evening, Philadelphia) with `curl`, not taken
from documentation. Exact commands are given so anyone can replay them. See also
`docs/research/septa-data-sources.md` (the design-phase research doc) for the broader survey;
this file only covers the two verifications this agent was specifically asked to do (subway
BusSchedules support, and the Alerts route-id prefix map), plus a few smaller things discovered
along the way while implementing the parsers.

## 1. BusSchedules is genuinely flaky, in a way that matters for parsing

```
curl -s -o /dev/null -w '%{http_code}\n' 'https://www3.septa.org/api/BusSchedules/index.php?stop_id=21297'
```
run 9 times in a retry loop returned `400, 501, 400, 501, 400, 501, 501, 501, 200` for the exact
same, valid, stop_id. The `501` responses were **not** `{"error": ...}` bodies - they were
ordinary, correctly-shaped schedule JSON (confirmed byte-for-byte plausible, e.g. one 501 body
returned real upcoming trips for stop 21297). Only a bad/missing `stop_id` reliably returns the
`{"error": "An invalid parameter was used. Ensure 'req1' or 'stop_id' is assigned a valid stop
id."}` shape, always with HTTP 400:
```
curl -s 'https://www3.septa.org/api/BusSchedules/index.php?stop_id=0'
curl -s 'https://www3.septa.org/api/BusSchedules/index.php'
```
both -> `{"error": "..."}`, HTTP 400.

**Consequence for the implementation**: `SeptaSource::fetchSchedule` (septa_source.cpp) parses
the response body whenever one came back, regardless of HTTP status - the status code alone
cannot distinguish "good data, mislabeled status" from "actually bad." `parseBusSchedules`
(septa.h/.cpp) is the thing that detects a real failure, by checking for the `{"error": ...}`
shape specifically. Retry/backoff across repeated polls is left to the HttpGet implementation
(`http_fetch.*`, not owned by this agent) per DESIGN.md 4.4/4.7.

Fixtures: `busschedules_21297.json` (fresh HTTP 200 body, refetched this session - the old
21297.json fixture predating this session was itself a captured HTTP 501 body with a
valid-shaped payload, exactly reproducing the pattern above; it is preserved as
`busschedules_error_501.json`). Added `busschedules_error_400.json`, a live-captured genuine
`{"error": ...}` body, since none of the pre-existing fixtures exercised that shape.

## 2. TransitView's "no data" shape differs from BusSchedules'

```
curl -s 'https://www3.septa.org/api/TransitView/index.php?route=ZZZZ999'   # -> [ ]  (HTTP 200)
curl -s 'https://www3.septa.org/api/TransitView/index.php'                 # -> full system-wide {"bus":[...]}, NOT an error
```
An unrecognized route returns a bare `[]`, not `{"bus": []}` and never `{"error": ...}`.
Omitting `route` entirely does **not** error - it silently returns the full unfiltered
system-wide feed (the ~129 KB `TransitViewAll` DESIGN.md 4.3 says never to fetch). `parseTransitView`
accepts both the normal `{"bus": [...]}` shape and the bare `[]` shape as a valid empty result;
`septaTransitViewUrl()` always includes `route=`, and nothing in this codebase ever omits it.

## 3. SEPTA's JSON is inconsistently typed

Verified field-by-field against `transitview_17.json` / a fresh TransitView capture:
`lat`/`lng` are JSON **strings** (`"39.950945"`), `Offset_sec` is a JSON **string** (`"792"`),
but `late`, `next_stop_sequence`, `Offset`, and `timestamp` are bare JSON **numbers**. Both
`BusSchedules` and `Arrivals` are consistently string-typed for everything they return. Rather
than assume one representation, every field access in `septa.cpp` goes through
`jsonToString`/`jsonToInt64`/`jsonToDouble` helpers that accept either.

Also observed: TransitView's `late` field uses **998/999 as "no live GPS fix" sentinels**
(e.g. a `VehicleID` of the literal string `"None"` paired with `"late":999` and
`"timestamp":63240`, which is not a plausible epoch - it's clearly a placeholder). `TvVehicle`'s
doc comment and `mergeStop()` (merge.cpp) both treat `|late| >= 900` as "not actually known"
rather than a real 999-minutes-late bus.

## 4. Alerts `[]` is not distinguishable from "wrong parameter"

```
curl -s 'https://www3.septa.org/api/Alerts/index.php?routes=nonsense_xyz'   # -> []  (HTTP 200)
```
Same footgun DESIGN.md 4.5 already flags for `req1=`: a syntactically-fine-but-wrong `routes=`
value returns the same empty array as "no current alerts for a real route." There is no way to
detect this from the response alone; the only mitigation is building `routes=` correctly in the
first place (see 7b below), which is what `alertRouteIdFor()` (septa_source.cpp) is for.

## 5. GTFS-RT TripUpdates fixture, decoded and cross-checked

```
python3 tools/gtfsrt_decode.py   # against firmware/test/fixtures/septa_bus_tripupdates.pb
```
confirms every value the task asked `GtfsRtStream` to reproduce: header `gtfs_realtime_version
"2.0"`, `timestamp 1789352333`; entity id `3667`, `trip_id "3667"`, `route_id "17"`
(via `TripDescriptor`), `direction_id 1`, vehicle id `"7477"`, and a `stop_time_update` at
`stop_sequence 35`, `stop_id "21332"`, `arrival.time 1789353562`. File stats: 317 entities,
148025 bytes total, largest single entity 2332 bytes (well under the 4096-byte default cap, so
none of the real fixture's entities get skipped - the oversized-entity path is only exercised by
a synthetic test case built in `test_gtfsrt_stream.cpp`).

## 6. Regional Rail station names: GTFS static names frequently do NOT work as Arrivals `station=`

Downloaded (`~750 KB`):
```
curl -s -o google_rail.zip 'https://www3.septa.org/developer/google_rail.zip'
```
`stops.txt` has 156 distinct station names. Tested every one against
`Arrivals/index.php?station=<name>&results=1` (URL-encoded, one request per name, ~50ms apart):
**137 of 156 worked as-is.** Corrected 13 more by inspection + one retry round (same physical
station, GTFS just abbreviates or prefixes differently) - e.g.:

| GTFS `stop_name` | Working Arrivals `station=` |
|---|---|
| `Gray 30th St Station` | `30th Street Station` |
| `9th St Lansdale` | `Lansdale` |
| `Airport Terminals E & F` | `Airport Terminal E-F` |
| `Chelten Av` | `Chelten Avenue` |
| `Chester Transit Center` | `Chester` |
| `Claymont Transit Center` | `Claymont` |
| `Highland Av` | `Highland Avenue` |
| `Newark DE` | `Newark` |
| `North Philadelphia Septa` | `North Philadelphia` |
| `Queen Ln` | `Queen Lane` |
| `Trenton Transit Center` | `Trenton` |
| `Washington Ln` | `Washington Lane` |
| `Wynnefield Av` | `Wynnefield Avenue` |

**Six names could not be resolved** despite retrying (3x each) and trying the obvious
abbreviation-expansions - these are real, currently-served stations, not typos, and are simply
excluded from `kRailStationNames` (see rail_stations.h) rather than guessed at:
`Airport Terminals C & D`, `Delaware Valley University`, `Fern Rock Transit Center`,
`Holmesburg Junction`, `Norristown Transit Center` (note: `Norristown Elm Street` is a
*different* station on the same line and works fine - it is in the list), `Richard Allen Ln`.
**This is the "anything you could not verify" item for task 6** - if the device ever needs one
of these six, its correct Arrivals station name will need discovering by someone with a way to
watch a live departure board, or by an EFF/SEPTA contact.

Final embedded list: 149 names, `firmware/lib/transit_core/src/rail_stations.cpp` and
`firmware/test/fixtures/rail_stations.json` (same list, JSON array, for the web mock).

## 7a. Subway (Broad Street Line) schedule-only support: CONFIRMED POSSIBLE, with a route-id gotcha

Downloaded (`~21 MB`):
```
curl -s -o google_bus.zip 'https://www3.septa.org/developer/google_bus.zip'
```
`routes.txt` shows the Broad Street Line and Market-Frankford Line are **not** called "BSL"/"MFL"
in GTFS static - they're `B1` (Broad St Local), `B2` (Broad St Express), `B3` (Broad-Ridge Spur),
and `L1` (Market-Frankford, all stops). `stops.txt` has `1286,Snyder,...` and
`1282,Walnut-Locust,...` (zone_id 50, the subway fare zone).

```
curl -s 'https://www3.septa.org/api/BusSchedules/index.php?stop_id=1286'
```
(retried past the same 400/501 flakiness as §1) returned real data:
```json
{"B1":[{"StopName":"Snyder","Route":"B1","trip_id":"325502","date":"10:41p","day":"Sun",
        "Direction":"0","DateCalender":"09/13/26 10:41 pm","DirectionDesc":"NRG"}, ...]}
```
**Yes - BusSchedules returns real scheduled subway trips, under route id `B1`, not `BSL`.**
Subway schedule-only display (DESIGN.md 4.6) is possible with zero extra work beyond what
BusSchedules already does for bus stops - `mergeStop()` doesn't require `SchedEntry::route` to
equal `StopConfig::route` for exactly this reason (see merge.h's comment).

Everything else about a subway station's route id stayed consistent with DESIGN.md's existing
note: `TransitView?route=BSL` and `Stops?req1=BSL` both still return `[]` (re-verified via the
existing `stops_BSL.json`/`transitview_BSL.json` fixtures plus a fresh check), and the bus
GTFS-RT feed carries no `B1`/`L1`/`BSL`/`MFL` trips (checked the 317-entity fixture in §5's
decode; no subway route ids present). So a subway StopConfig gets **schedule only**, sourced
purely from BusSchedules with the physical station's own `stop_id` - exactly DESIGN.md 4.6's
fallback design, now confirmed rather than assumed.

**Not fully verified**: only `Direction "0"` (toward NRG/South Philadelphia) came back for stop
1286 across several retries; no second stop_id for the Fern Rock-bound direction at the same
physical station was found in this session (subway platforms may need distinct stop_ids per
direction the way bus stops on opposite sides of the street do - `stops.txt` didn't have an
obviously-named second "Snyder" entry). A two-direction subway display will need whoever builds
the stop-picker UI to find/verify the opposite-direction stop_id per station.

Fixture added: `busschedules_bsl_1286.json` (used by the schedule-only merge test).

## 7b. Alerts route-id prefixes: fetched the full unfiltered list and read off every prefix in use

```
curl -s 'https://www3.septa.org/api/Alerts/index.php'
```
(no params - this is the URL the Swagger spec itself says is the authoritative source of valid
route ids: `"description": "...Get all the route id from https://www3.septa.org/api/Alerts"`)
returned 185 entries, 138560 bytes. Distinct `route_id` prefixes actually seen, with `mode`:

| Prefix | Count | `mode` values seen | Example `route_id` -> `route` |
|---|---|---|---|
| `bus_route_` | 155 | `Bus` | `bus_route_17` -> `17`, `bus_route_BLVDDIR` -> `BLVDDIR`, `bus_route_G`/`H`/`J`/`K`/`L`/`R`/`XH` -> the New Bus Network letter routes, `bus_route_LUCY`/`LUCYGO`/`LUCYGR` |
| `trolley_route_` | 8 | `Trolley` (and, oddly, one `Bus`: `trolley_route_15` has `"mode":"Bus"` - route 15 is currently bus-substituted but keeps its historical trolley route-id prefix) | `trolley_route_10`, `_11`, `_13`, `_15`, `_34`, `_36`, `_101`, `_102` - the legacy numbered trolley network |
| `rr_route_` | 19 | `Regional Rail`, **`Broad Street Line`, `Market/ Frankford`, `Norristown High Speed Line`** | `rr_route_chw` -> `CHW`, ... **and, unexpectedly, `rr_route_bsl` -> `BSL`, `rr_route_bso` -> `BSO`, `rr_route_mfl` -> `MFL`, `rr_route_mfo` -> `MFO`, `rr_route_nhsl` -> `NHSL`** |
| `cct` | 1 | `CCT` (Customized Community Transportation, paratransit) - no numeric suffix at all |
| `generic` | 1 | route `SYS`, system-wide advisory |
| `APP` | 1 | `Mobile` - about the SEPTA app itself, not a route |

**The single most important finding: subway alerts use the `rr_route_` prefix, not
`bus_route_`.** Confirmed directly (not just inferred from the full listing):
```
curl -s 'https://www3.septa.org/api/Alerts/index.php?routes=rr_route_bsl'   # -> real alert object, HTTP 200
curl -s 'https://www3.septa.org/api/Alerts/index.php?routes=bus_route_BSL'  # -> [] (wrong prefix)
```
`alertRouteIdFor(Mode::Subway, route)` (septa_source.cpp) builds `rr_route_<lowercase route>`
accordingly - the one mode where the alert prefix does NOT match the mode's own "family"
(bus-like everywhere else).

**Regional Rail line codes need a lookup table, not a formula.** The `route` short codes in the
Alerts listing (`AIR, CHE, CHW, CYN, FOX, GLN, LAN, MED, NOR, PAO, TRE, WAR, WIL, WTR`) don't
`lowercase()` cleanly into their `rr_route_` suffixes: `TRE -> rr_route_trent` (not `_tre`),
`WTR -> rr_route_wtren`, `WIL -> rr_route_wilm`, `LAN -> rr_route_landdoy`, `FOX -> rr_route_fxc`,
`GLN -> rr_route_gc`. Cross-referenced each surviving code against real `line` values seen in
`arrivals_30th.json`/`trainview.json` (`Fox Chase`, `Warminster`, `Media/Wawa`, `Trenton`,
`Airport`, `West Trenton`, `Lansdale/Doylestown`, `Manayunk/Norristown`, `Paoli/Thorndale`) plus
the two not seen live but standard SEPTA line names (`Chestnut Hill East`/`West`, `Cynwyd`,
`Wilmington/Newark`) to build `kRailLines` (septa.h/.cpp) - the one table both `mergeRail()`
(line filtering) and `alertRouteIdFor(Mode::Rail, ...)` (Alerts URL) use.
**`GLN`/`rr_route_gc` is excluded** - it never appeared as a `line` value in any Arrivals/
TrainView response seen this session (only as a *destination*, for a Warminster-line train), so
there's no confirmed display name to map it to; **unverified**, flagged rather than guessed.

**Also not fully verified**: the new lettered/T-prefixed trolley routes (`T1`-`T5`, confirmed
live via `TransitView?route=T1..T5` -> real vehicles, `mode: "Bus"`) and the New Bus Network
letter routes `G1`/`D1` had **no current alert entry at all** in the full unfiltered listing, so
neither `bus_route_T4` nor `trolley_route_T4` could be confirmed against a real alert (both
return `[]`, which - per the footgun above - proves nothing either way). `alertRouteIdFor`
follows DESIGN.md's own stated convention (`Mode::Trolley -> trolley_route_<route>`), which
**is** confirmed correct for the legacy numbered trolleys (10/11/13/15/34/36) still active in the
Alerts feed; whether it also holds for `T1`-`T5` should be re-checked once one of them has a
live alert to test against.

## Summary of decisions this drove in the code

- `parseBusSchedules`/`parseTransitView`/`parseRailArrivals` all check for `{"error": ...}`
  explicitly rather than trusting HTTP status (§1, §2).
- `TvVehicle::late` documents the 998/999 sentinel; `mergeStop()` treats `|late| >= 900` as
  "not known" (§3).
- `mergeStop()` matches `SchedEntry` by direction only, never by route id, so subway's `B1` vs.
  `BSL` mismatch (§7a) doesn't break schedule-only display.
- `alertRouteIdFor()` special-cases `Mode::Subway` to the `rr_route_` prefix (§7b) and uses the
  `kRailLines` table (not a formula) for `Mode::Rail` (§7b).
- `kRailStationNames` omits the six station names that don't work, rather than guessing (§6).
