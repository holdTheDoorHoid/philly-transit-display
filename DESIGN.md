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
| Transport | Plain HTTP in every shipping env. The opt-in HTTPS mode (Amazon Trust CA bundle, `tls_verify`) shipped in v0.1.0-0.1.1 and was removed in v0.1.2 to free flash. Re-examined with measurements on 2026-09-16 (§2.1): a heap-gated "verified HTTPS when it fits, plain HTTP otherwise, never a downgrade after a TLS failure" prototype exists behind `-DTRANSIT_HTTPS` (envs `cyd-*-https`) with three pinned root CAs and a `device.transport` policy, and on the owner's board the gate never found room - the byte-addressable heap at the fetch point is 27-42 KB against a session's ~55 KB. It stays a prototype until the SDK is rebuilt with smaller TLS buffers; §2.1 has the numbers and the recipe. |
| Log format | Append-only CSV, one file per month on SD, one event per line | Human-readable, spreadsheet-friendly, streamable with tiny RAM. |
| Stats computation | On device, streaming over CSV with fixed-size histograms | 320 KB RAM, no PSRAM; never load a month of log into memory. |
| Repo/License | `holdTheDoorHoid/philly-transit-display`, MIT | Owner's choice. |

### 2.1 HTTPS to the data sources: measured on the board, and why it stays a prototype (2026-09-16)

Review finding F04 wanted the device to fetch SEPTA, Open-Meteo and Indego over verified HTTPS and
to stop silently rewriting `https://` to `http://`. The owner deferred it to its own release; this
section is that work: what the servers offer, what TLS costs on this exact SDK (measured, not
quoted), a heap-gated prototype behind `-DTRANSIT_HTTPS`, and what happened when it ran on the
owner's board. Nothing here changes a shipping image: the prototype is compiled only by the
`cyd-*-https` envs in `platformio.ini`.

**In plain language.** An encrypted connection needs the board to set aside about 42 KB of memory
for as long as the connection is open, in two pieces of 16.7 KB that each have to be one unbroken
block, and about 12 KB more for a second or two while the connection is being set up. The board
*reports* 70-80 KB free while it runs, but about 34 KB of that is a kind of memory the processor can
only read in whole words, which no buffer, string or network record can use; the memory an ordinary
allocation can actually get is about 35-39 KB free at idle and 27-42 KB at the moment a fetch starts.
(The "46 KB at idle" this paragraph carried until 2026-09-16 came from an instrumented prototype
build and could not be reproduced; two independent measurements on the owner's own configuration
agree on the lower figure. It does not change the conclusion below - it widens the gap.)
So on this board, with the software the ESP32 vendor ships, HTTPS does not fit - not sometimes,
never: the prototype asked for it on every fetch for the whole test and was refused every time,
while the arrivals kept flowing over plain HTTP exactly as before. The obvious cheap fix - do the
encrypted fetch at a quieter moment of the poll instead - was measured rather than assumed, and
there is no quieter moment: the whole poll cycle sits within 6 KB of itself, so the best instant in
it is still about 27 KB short, and a run with the memory check stripped to the bare minimum proved
it by trying anyway and failing. The honest design is therefore:
keep the shipping firmware as it is (plain HTTP, no TLS code, the flash and stack it would cost
left for the display), keep the prototype for the day the vendor library is rebuilt with smaller
TLS buffers (the one change that would make HTTPS fit), and never let a certificate problem turn
into a plain-HTTP fetch.

#### What the three servers offer (probed with `openssl s_client`, 2026-09-16)

| Host | Chain the server sends (leaf ← intermediate ← root) | Keys | TLS | Suite this SDK will get | MFL 4096 | `Connection: close` |
|---|---|---|---|---|---|---|
| `www3.septa.org` | `*.septa.org` ← Amazon RSA 2048 M01 ← **Amazon Root CA 1** (all three sent; the root copy is cross-signed by Starfield G2) | RSA-2048 ×3 | **1.2 only** | ECDHE-RSA-AES128-GCM-SHA256, P-256 | ignored | honoured (`Connection: Close`), Content-Length bodies (TripUpdates was 79 KB at 01:30, ~150 KB by day) |
| `api.open-meteo.com` | `open-meteo.com` ← YR2 ← Root YR (cross-signed by **ISRG Root X1**) | RSA-2048, RSA-2048, RSA-4096 | 1.3; 1.2 offered | ECDHE-RSA-AES128-GCM-SHA256 (X25519 or P-256) | **honoured** (the only one) | honoured, chunked body |
| `bts-status.bicycletransit.workers.dev` | ← GTS WE1 ← **GTS Root R4** (cross-signed copy by GlobalSign Root CA, which expires 2028-01-28; the self-signed root we pin runs to 2036) | ECDSA P-256, P-256, P-384 | 1.3; 1.2 offered | ECDHE-ECDSA-AES128-GCM-SHA256 (Cloudflare prefers ChaCha20-Poly1305, which this SDK does not compile) | ignored | honoured |

All three still answer identically over plain `http://` (200s, same bodies), so nothing forces the
change today. Only SEPTA is stuck on TLS 1.2, which happens to be all this SDK speaks (TLS 1.3 is
compiled out - `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3` unset). SEPTA's chain is RSA end to end: three
RSA-2048 signature checks per handshake, on the ESP32's bignum accelerator, plus one ECDHE P-256
agreement in software. Indego's is ECDSA end to end, the cheapest of the three; ECDSA versus RSA
changes handshake CPU time (hundreds of ms either way), not the memory that decides this question.

**Pinned roots** (`firmware/src/app/tls_roots.cpp`, generated by `tools/gen_tls_roots.py`):
Amazon Root CA 1 (1,188 B PEM, RSA-2048, valid to 2038-01-17), ISRG Root X1 (1,939 B, RSA-4096, to
2035-06-04), GTS Root R4 (765 B, ECDSA P-384, to 2036-06-22): **3,892 B of flash** against the
SDK's ~70 KB full bundle. Each served chain was verified against exactly its one root with the
system store disabled (`openssl verify -no-CApath -no-CAfile -no-CAstore -CAfile <root> -untrusted
<served intermediates> <leaf>`: all three OK; the SEPTA leaf against ISRG Root X1 fails, as it
must). One root is handed to mbedTLS per host (`tls_roots.h`), so the pin is per source, not "any
of these three".

**Rotation risk, plainly.** The roots outlive the hardware; the risk is a source changing CA -
SEPTA moving off AWS's CA, Open-Meteo leaving Let's Encrypt (its current Root YR is itself new in
2026 and only reaches us cross-signed by ISRG Root X1), Bicycle Transit leaving Cloudflare. That
day, TLS to that host fails *closed* with a certificate error, `/api/state`'s `transport.cert_failed`
climbs, and the fix is a firmware update (`tools/gen_tls_roots.py <new root>.pem`, one line in
`tls_roots.h`). Under `https_preferred` the display keeps working meanwhile only to the extent the
heap gate was already sending that fetch over plain HTTP - a bad certificate is never a reason to
downgrade. Under `https` the stop reads unavailable until the update. Two more limits of this SDK
to state: certificate *dates* are not checked at all (`CONFIG_MBEDTLS_HAVE_TIME_DATE` unset, so an
expired certificate that chains to the pinned root still verifies), and a revoked one is not either
(no OCSP/CRL, as on every embedded client of this class). The review's acceptance test "an expired
certificate fails closed" cannot be met without rebuilding the SDK.

#### What one TLS session costs on this SDK (Arduino-ESP32 3.2.1 / ESP-IDF 5.4.2, mbedTLS 3.6.3)

Read off the precompiled SDK's `sdkconfig` and `esp_config.h`, then confirmed with a `sizeof` probe
compiled by the xtensa toolchain against those same headers:

| Item | Value | Consequence |
|---|---|---|
| `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` | 16384, IN = OUT (`ASYMMETRIC_CONTENT_LEN` unset) | `MBEDTLS_SSL_IN_BUFFER_LEN` = `OUT` = **16,717 B**, calloc'd back to back by `mbedtls_ssl_setup()`, held for the whole session |
| `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` | unset | the buffers are never resized after the handshake |
| `MBEDTLS_SSL_MAX_FRAGMENT_LENGTH` | compiled in (unconditional in `esp_config.h`) | the client *could* negotiate MFL 4096 - but without the line above that only changes what the server sends per record, not what we allocate; and only Open-Meteo honours it anyway. `NetworkClientSecure` exposes no MFL call. |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | absent | no "free the buffers between records" mode |
| `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` | y | the peer's parsed chain (~6 KB for SEPTA's three RSA certificates) stays allocated for the session |
| `CONFIG_MBEDTLS_HAVE_TIME_DATE` | unset | no validity-date checks (above) |
| `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3` | unset | TLS 1.2 client; fine for all three hosts |
| `CONFIG_MBEDTLS_ECP_RESTARTABLE` | unset | ECDHE cannot yield mid-computation; each step runs to completion on the poller task |
| `CONFIG_MBEDTLS_HARDWARE_{MPI,SHA,AES}` | y | RSA verify and AES-GCM are accelerated; ECP point math is software with `ECP_NIST_OPTIM` |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` / `CHECK_IDLE_TASK_CPU0` | 5 / y | the same watchdog the plain path already works around |
| sizeof: `mbedtls_ssl_context` / `ssl_config` / `entropy` / `ctr_drbg` / `x509_crt` / handshake params / transform / session | 552 / 196 / 420 / 76 / 408 / 944 / 220 / 144 B | `NetworkClientSecure`'s constructor `new`s ~2.1 KB before a byte is sent |

**Peak-heap estimate for one handshake plus one streamed body**, from those numbers: 2.1 KB of
contexts + 33.4 KB of record buffers + ~1.3 KB of handshake/transform/session structs, plus during
the handshake ~2-2.5 KB for the parsed pinned root (freed by `ssl_client.cpp` right after
verification), ~6 KB for SEPTA's parsed chain (kept), and ~3 KB of ECDHE/RSA bignum scratch: **~52-55
KB at the peak, ~42 KB steady while the body streams**, on top of `HTTPClient`'s 1.4 KB read buffer
and whatever the consumer allocates (the GTFS-RT decoder's entity buffer and `rt_updates`). The
folklore "40 KB per TLS session" is right for this SDK, and it is not a number the Arduino API can
lower: `NetworkClientSecure` lets us set the handshake timeout (default 120 s - the prototype uses
8), the connect timeout (it reaches `select()` and `SO_RCVTIMEO`), and one root PEM; nothing else.

**Watchdog and blocking.** `ssl_client.cpp` makes the socket non-blocking and never changes it
back. The TCP connect waits in `select()` (a real block; IDLE0 runs). The handshake loop calls
`mbedtls_ssl_handshake()` until it stops saying WANT_READ, with `vTaskDelay(2)` between rounds, so
the idle task is fed; the compute-heavy rounds (ECDHE keygen and agreement, three RSA-2048
verifications) run inline for well under a second each, inside the 5 s budget. Body reads go through
the same `Stream::timedRead()` busy-wait the plain path already lives with, capped by the existing
`kStreamReadTimeoutMs` (4 s). What a TLS handshake adds is **stack**: ~3-4 KB on the poller task
(mbedTLS's RSA verify keeps a 1 KB buffer on the stack, `ssl_starttls_handshake` 512 B, ECP
temporaries) - the poller has a 10 KB stack (`net_poller.cpp`), and the device test below reads
its high-water mark first.

#### Flash and RAM, measured (2026-09-16, this branch after merging `next`)

| Build | `cyd-3248S035R` flash | Δ | static RAM | Δ |
|---|---:|---:|---:|---:|
| shipping env, flag off (`next` merged: exception pool, streamed `/api/state`, Settings blocks) | 1,841,390 B (96.9 %) | - | 95,652 B | - |
| `cyd-3248S035R-https`: `-DTRANSIT_HTTPS` + pinned roots + `CORE_DEBUG_LEVEL=0` + the heap trace | 1,859,498 B (97.8 %) | +18,108 B | 96,172 B | +520 B |
| `cyd-2432S024C` shipping, flag off | 1,836,966 B (96.7 %) | - | 95,788 B | - |
| `cyd-2432S024C-https` | 1,854,338 B (97.6 %) | +17,372 B | 96,308 B | +520 B |

All six board envs and both prototypes build. Flash headroom on this branch, for the record - it is
what an SDK-rebuild attempt has to fit into, and it is not the same on every board:

| env | flash | headroom |
|---|---:|---:|
| `cyd-2432S028Rv3` | 1,831,934 B | 68,610 B |
| `cyd-2432S028R`, `cyd-2432S024R` | 1,832,998 B | 67,546 B |
| `cyd-2432S024C` | 1,836,966 B | 63,578 B |
| `cyd-3248S035R` (the owner's) | 1,841,390 B | 59,154 B |
| `cyd-3248S035C` | 1,860,586 B | **39,958 B** |
| `cyd-2432S024C-https` | 1,854,338 B | 46,206 B |
| `cyd-3248S035R-https` | 1,859,498 B | 41,046 B |

The shipping envs are ~1.1 KB larger than `next` and that is not TLS: the web app is one bundle for
every firmware, so the "Data link" tile, `describeTransport()` and the "Data connection" field ship
in the gzipped `web_assets.h` of every board and hide themselves at runtime when `/api/state` has
no `transport` block. That is the right trade - one web app, no per-build variants - but it is the
only thing in this section a shipping image pays for.

The +18 KB is two things netted: TLS itself is about **+75 KB** (`libmbedtls.a`'s record and
handshake layer, `libmbedx509.a`, `NetworkClientSecure`, the three roots at 3.9 KB, the gate) after
the two link-time trims in `http_fetch.cpp` (mbedTLS's 16 KB error-string table and its 7.9 KB TLS
*server* stepper are kept out of the image), and `CORE_DEBUG_LEVEL=0` is about **-57 KB** (level 1
still compiles every `log_e()` format string and call site in the Arduino core - HAL, WiFi, Network,
SD, AsyncTCP, Updater - plus libc's `strerror` table). `device.transport` is behind the flag down to
the `DeviceConfig` member itself, so a shipping image carries no `std::string` (nor its literal,
constructor, destructor and copies) that nothing in that build can read: the flag-off env is what
`next` builds plus the `[poll-heap]` trace's empty inline function, which costs nothing.

**What `CORE_DEBUG_LEVEL=0` silences, exactly.** Every `[tag]` line this project prints is a plain
`Serial.printf` and is unaffected: `[heap]`, `[net_poller]`, `[https]`, `[auth]`, `[proxy]`,
`[weather]`, `[bike]`, `[main]`, `[lvmem]`. What goes quiet is (a) the Arduino core's own
`[E][file.cpp:line]` lines - Wi-Fi association failures, SD card and LittleFS errors, `Updater`
errors, AsyncTCP and HTTPClient errors - and (b) this project's 17 `log_e()` sites, all already
visible some other way: `config_store.cpp` (10: a config that fails to open, parse or validate at
boot, and the save path's open/write/verify/rename failures - the boot case is reported as
`config_recovered` in `/api/state`, the save case as the HTTP error the PUT returns), `sd_logger.cpp`
(2: append/short-write failures, reported as `sd.write_ok` / `sd.error`), `main.cpp` (3: mDNS
start failures, and the tracker allocation failure that disables logging), `auth.cpp` (1: NVS
namespace unavailable, boot-only PIN) and `http_fetch.cpp` (1: `begin()` rejected the URL). The
prototype envs pay that; a shipping env does not, because nothing in this section makes it worth
paying.

#### The heap, measured properly (the finding that decides this)

Every heap number this project had written down - the README's stage table, `/api/state`'s `heap`,
the 60 KB OTA gate, the 24 KB `/api/state` gate, the 40 KB idle-work gate, §12.1's "~75-80 KB free"
- was `ESP.getFreeHeap()`, which is `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`
(`cores/esp32/Esp.cpp`). On the classic ESP32 that includes the IRAM heap region left over after
the app's IRAM code, which is **32-bit-word addressable only**: `malloc()` never hands it out for a
buffer, a `std::string`, a `JsonDocument` or a TLS record (those come from `MALLOC_CAP_8BIT`).
Measured on the owner's board with both numbers printed side by side (the `[heap]` boot lines and
the `[net_poller]` heartbeat now carry `free8=`):

| Point | `ESP.getFreeHeap()` (INTERNAL) | `MALLOC_CAP_8BIT` free | largest 8-bit block |
|---|---:|---:|---:|
| after display init | 204,212 | 170,504 | 110,580 |
| after tasks | 174,240 | 140,532 | 110,580 |
| poller start, first poll (`poll-start` / `pre-fetch` probes) | 82,080 / 81,348 | - | 47,092 / 45,044 |
| the TripUpdates gate of that first poll, ~9 KB of decoder later | - | 37,808 | 36,852 |
| the TripUpdates gate, steady state (every later poll) | ~70,000 at the heartbeat | 26,600-29,000 | 13,300-18,400 |
| the later fetches of a poll (TransitView, BusSchedules, Alerts, Open-Meteo, Indego) | - | 23,700-41,900 | 13,300-27,600 |
| every gate decision of the 21-poll soak, second run, for comparison | - | 27,128-42,372 (median 29,748) | 18,420-29,684 |

The IRAM-only share is ~34 KB throughout. So the byte-addressable heap at the moment a fetch begins
is **27-42 KB with an 18-30 KB largest block**, against a TLS session's 33.4 KB of contiguous record
buffers plus ~20 KB around them. The gate (`kTlsHeapNeed` = 68,250 B of 8-bit heap, and the two
16,717 B blocks actually allocated and freed as the test) refused **every one of the 64
fetches** that asked for `https://` during the test, and it was right to: forcing it (below) shows
what happens when TLS is attempted anyway. This is not a fragmentation problem a reboot fixes - the
first fetch after a fresh boot, with the cleanest heap the device ever has, was 37.8 KB short.

The same number explains two things the project had put down to other causes: the "~54 KB minimum
during a poll" of §12.1 is ~20 KB of real heap, which is why `/api/state` answers 503 during a plain
HTTP fetch, and why the OTA gate ("largest free block 15 KB is below 16 KB") refused every upload
attempt for ten minutes on 2026-09-16 while the device was polling every 15 s - a reboot followed by
an immediate upload, or the serial cable, is the way in. The gates themselves are tuned to the
INTERNAL number and keep working; a follow-up should restate them in 8-bit terms.

#### The follow-up: every gate re-derived in 8-bit terms (2026-09-16)

Done, and two of the three gates turned out not to have been working at all. The IRAM share is not
"about 34 KB", it is **33,708 B**, and it is a fixed region rather than a moving figure - sized once
when the app's IRAM code is placed, never allocated from. Paired `free`/`free8` readings on the
owner's cyd-3248S035R, across two firmware builds and every point from `display` to a poll in
flight, give that difference; a second agent independently got the same 33,708 B across three boots,
against a map-derived upper bound of 34,561 B whose extra ~853 B is IRAM tail actually in use. That
makes the restatement exact rather than approximate - and it makes the consequence exact too.
`MALLOC_CAP_INTERNAL` free **never drops below 33,708 B**, so any gate whose floor sat under that
number could not fire:

| Gate | Was | In real (8-bit) terms | Could it fire? | Now |
|---|---|---|---|---|
| `/api/state`, `/api/config` | 24 KB INTERNAL free, 8 KB block | ~0 KB of usable heap | **never** | 12 KB `free8`, 7,924 B block |
| poller idle slice | 40 KB INTERNAL free, 12 KB block | ~6 KB of usable heap | **almost never** | 16 KB `free8`, 12,020 B block |
| OTA admission | 60 KB INTERNAL free, 16 KB block | ~26 KB of usable heap | yes, constantly | 16 KB `free8`, 5,876 B block |

The first two had been running on their largest-block halves alone, which is why they behaved
sensibly despite the free half being unreachable - the comments described a check that was not
happening. The third had the opposite problem: 26 KB of byte-addressable heap sits *inside* the
26.6-29 KB band a poll leaves behind, so the gate flapped with the poll cycle, and the 16 KB block
requirement was **4x** anything the OTA path allocates. `Update.begin()` takes exactly one
`new uint8_t[SPI_FLASH_SEC_SIZE]` = 4,096 B (Arduino's `Updater.cpp`); the only other allocation on
the path is a 16 B `_skipBuffer`. Nothing in `Update.write()` is body-sized - ESPAsyncWebServer
delivers the multipart body in ~1.4 KB pieces and Update accumulates them into that one sector.

Each new threshold is derived from what its path actually allocates, not from a round number -
and the *block* halves are then moved off the 512-byte lattice largest-block sizes land on (the
`kMinOtaLargestBlock` comment in `web_server.cpp` carries that measurement), which is why they read
5,876 / 7,924 / 12,020 rather than 6 / 8 / 12 KB. Where this document quotes a threshold it quotes
the constant, in bytes: a rounded restatement is how the pre-lattice figures survived a whole
release in four places here after the code had moved (found in the RC review, 2026-09-16).

- **OTA** - one 4,096 B sector buffer, so a 5,876 B block (1.43x) and 16 KB of `free8` for the
  request machinery and for the rest of the device to keep running through a ~1.7 MB upload.
- **`/api/state` / `/api/config`** - a Config copy, a Snapshot copy, ArduinoJson's 1 KB slot pools
  and string pool for a ~4 KB document, and one 2,872 B send buffer
  (`ASYNC_RESPONCE_BUFF_SIZE` = `CONFIG_LWIP_TCP_MSS * 2`). The 7,924 B block is 2.76x that
  buffer; the 12 KB free floor is deliberately *below* the full transient cost, because §12.1's
  reasoning still holds - the floor exists to skip a hopeless build, not to promise a successful
  one.
- **Idle slice** - the ~8 KB contiguous `StatsAggregator`, so a 12,020 B block (~1.5x) and 16 KB
  of `free8`. The "4 KB proxy write buffer" the old comment cited is not in the sum: it is
  `static uint8_t wbuf[4096]` and never comes off the heap.

Measured against 114 samples of ordinary polling on the owner's board - two stops, the owner's own
config, counting only samples where a poll had completed and the device had not just rebooted - the
OTA gate's admission rate goes from **66% to 96%**. Every one of the remaining refusals is the
`free8` half, at moments when `free8` is genuinely down near its 11.0 KB floor with a largest block
to match. The block half never refused once at 6 KB, against a measured block floor of 8,692 B. The
4% that still refuse are the heap actually being busy, which is the case the gate is for; the 34%
that used to refuse were an accounting error.

Two cautions on those percentages. Fresh-boot samples are excluded deliberately: right after a
reboot the device has ~48 KB of `free8` and a 47 KB largest block, the cleanest heap it ever has, so
including them flatters every gate and hides exactly the mid-poll case that was failing. And the
ceiling matters as much as the floor - `free8` at healthy idle tops out at **36.6 KB** here, with a
second agent independently measuring 36.8-39.3 KB on a clean build with the same config. Any gate
restated by moving the old INTERNAL number across unchanged would land above that ceiling and be
permanently false: the 40 KB idle-work floor in particular would never once have been satisfied.
That is the trap in this whole class of fix, and it is why each threshold above is derived from an
allocation rather than converted from its predecessor.

The alternative fix for the OTA case - having the handler wait for the poller's idle gap - was
rejected. `index == 0` runs on the AsyncTCP task, and blocking there stalls every other connection
on the device, which is a worse failure than the one being repaired, while the client is already
mid-upload with ~1.7 MB to push.

**Verified on the owner's board, 2026-09-16.** Not inferred from the sample distribution: the gate
was made to decide, on hardware, at the moments it used to refuse. A firmware image carrying valid
ESP32 magic and no board marker exercises the admission gate at `index == 0` and is then refused by
the board-marker check before `Update.end(true)`, so it never becomes bootable - which makes "would
this upload have been let in?" a question you can ask a live device repeatedly and safely. `400`
("firmware is for a different board") means the gate admitted it; `503` means the gate refused it.
Four such probes, each fired 1-4 s after a poll completed - precisely the window that used to
refuse - all came back `400`, with `free8` at 38,888-40,104 B and the largest block at 23,540 B.
The full run: 14 of 15 checks passed, the fifteenth being the `heap`/`heap_8bit` sampling artefact
described above, which was then characterised rather than dismissed. The owner's two stops stayed
live with four arrivals each throughout, the panel stayed on the main page, and no config was
written at any point.

There is one failure mode this does **not** repair, and it is worth stating so the fix is not read
as more than it is. Two builds were found unable to take an OTA *at rest* - not a dip during a
poll, their steady state - both measured on the owner's two-stop config on 2026-09-16:

| Build | Resting largest block | Against the old 16,384 B gate | Attempts |
|---|---:|---|---|
| `3707f54` | 11,764 B | 4,620 B short | 5 over 10 min, all refused |
| released **v0.2.0** | 16,372 B | **12 B short** | 3, all refused |

The v0.2.0 row misses by twelve bytes, and that number is not a coincidence - it is the signature of
a threshold placed on a lattice boundary.

**`heap_caps_get_largest_free_block()` returns values on a 512-byte lattice at offset 500.** Every
one of 24 distinct values measured here fits `500 + 512k` exactly, and two other agents confirmed
the same structure independently - 22 of 25 and 27 of 30 distinct values on separate captures across
four images, the misses in every set being deep mid-poll readings.

The rule that follows is one line of arithmetic, and it is more general than "avoid round
kilobytes": **any multiple of 512 sits exactly 12 B above a lattice point, because 512 - 500 = 12.**
The point below `512m` is `500 + 512(m-1)` = `512m - 12`, for every m. Verified exhaustively over
`512m` from 1,024 to 19,968: the margin is 12 B every single time, never more, never less. So a
build resting on a lattice point is refused by a hair while the next value up clears by 1,012 B, and
a "narrow miss" against such a threshold is *always* a 12-byte miss - never a comfortable margin,
because the nearest failing value below is 1,036 B short. Nothing about v0.2.0's twelve bytes is
accidental, and calling it a coin toss (as an earlier draft of this section did) gets the mechanism
backwards.

All four round thresholds in play had it, with the bottom two observed at rest on real devices:

| Threshold | Lattice point below | Short by | Next value up | Observed resting there |
|---|---:|---:|---:|---|
| 16,384 old OTA block | 16,372 | 12 | 17,396 | yes, v0.2.0, 8 minutes |
| 12,288 idle-work block | 12,276 | 12 | 13,300 | yes |
| 8,192 `/api/state` block | 8,180 | 12 | 9,204 | yes |
| 6,144 new OTA block | 6,132 | 12 | 7,156 | - |

Fixed by moving all three block thresholds mid-gap: **5,876** (OTA), **7,924** (`/api/state`) and
**12,020** (idle slice), which are `756 + 512k` and therefore 256 B from either neighbour. A device
resting on any lattice point is now admitted or refused with real margin, and a small change in
allocation cannot flip admission. The derivations did not need redoing - each is still comfortably
above what its path allocates (1.43x, 2.76x and 1.47x respectively); they needed moving off the
boundary.

Note that `m * 1024 - 512` does **not** fix this, which is worth stating because it is the obvious
correction and it was the first one proposed: 5,632, 7,680 and 11,776 are themselves multiples of
512, so they sit 12 B above 5,620, 7,668 and 11,764 respectively - the same pathology one residue
over. That correction only looks mid-gap under a two-family model of the lattice, which is what a
1024-byte window shows you: `500 + 512(2j)` has residue 500 mod 1024 and `500 + 512(2j+1)` has
residue 1012, odd and even terms of one sequence seen through a window twice its period. Inferring
a period from too narrow a window is the actual error here, and it is worth naming because the
mid-gap calculation depends on exactly that quantity.

The free-size thresholds are unaffected: `free8` is a sum over every free block, not one contiguous
run, and it does not lattice. Checked rather than assumed - 128 distinct `free8` values from serial
captures are 4-byte aligned and nothing more, with 81 distinct residues mod 512 and 107 mod 1024.
So `kMinOtaFree8`, `kMinHeavyResponseFree8` and `kIdleWorkMinFree8` stay where their allocation
derivations put them.

**Known gap:** four readings across two independent sample sets do not fit the lattice - 2,932 here,
and 1,396 / 4,340 / 5,876 elsewhere. All four are deep mid-poll values taken while the heap is
actively churning, and none is a resting value, but that is exactly when a gate may be consulted.
The lattice is a reliable description of resting behaviour and an incomplete one under churn, so do
not assume a mid-poll refusal lands on a lattice point.

None of this should be over-read as "shipped devices are lockable". It is one configuration,
measured. What it does establish is that a *different* configuration resting one lattice point lower
would have been locked out just as precisely.

The new 6 KB threshold admits both boards with real headroom, because it comes from the single
4,096 B buffer the update path allocates rather than from a round number. But a sufficiently
long-lived device can always fragment past any floor, so every OTA refusal now names the way out
instead of being a dead end. The restart is not taken automatically: this is a display on someone's
wall, and a failed upload is not a reason to blank it.

The escape window is build-dependent and can be short, which is why the message says "within the
first minute" rather than just "reboot": `3707f54` reboots to 23,540 B and stays there, so the retry
can happen at leisure, but v0.2.0 returns to its resting 16,372 B about 45 s after boot once the
poller runs. An upload at uptime 13 s returned 200. USB flashing always works and is the answer for
anyone already stranded.

One reporting lesson from the same measurement, applied to these messages: the old refusal read
"largest free block 15 KB is below 16 KB" on the device that was twelve bytes short. Integer
division turned a knife edge into what sounds like a comfortable kilobyte, and sent the reader
hunting for what was eating 1 KB. **Any number a human is expected to act on is reported in bytes.**

Nor does any of this address a heap that is decaying toward zero. If `free8` runs down far enough,
lwIP asserts on `MEMP_SYS_TIMEOUT` exhaustion before the poller's own failure counting reaches its
wedge threshold, and no admission floor prevents that. What the change does do in that regime is
shed optional work earlier than before, because the two gates that could not fire now can - a softer
landing, not a cure.

#### One visible consequence: `/api/state` now answers 503 during an OTA (measured 2026-09-16)

Waking a gate that could never fire means it fires. Sampling `/api/state` once a second through a
full 1.85 MB upload on the owner's board: fourteen consecutive `200`s, then six consecutive
`503 {"error":"low memory, retry"}`, then recovery. An in-flight OTA holds a large sustained
allocation - the same sampling put `free8` at least 21 KB below its idle baseline - and it pushes
the byte-addressable heap under the 12 KB floor for several seconds. The old gate could not see
this: its free half was 24 KB of `INTERNAL`, which is *below zero* in 8-bit terms, and the largest
block stayed at 10.7-13.8 KB, above the 8 KB half that did work. So `/api/state` used to keep
answering here, and now it does not.

That is the right behaviour, not a regression to tune away. During an upload the heavy read handler
is competing with `Update` for the last few KB of usable heap, and a failed flash is far worse than
a status endpoint that is briefly unavailable - a 503 is cheap, fixed-literal, and retried. The gate
backing off *protects the upload*. Clients should expect it: the web app keeps its last data and
retries rather than blanking, and any script polling `/api/state` through an OTA must treat 503 as
"retry", exactly as the low-memory contract has always said.

**A gated endpoint cannot report the condition that gates it.** This caught the investigation above
and it is general, so it belongs here rather than in a footnote. An earlier pass reported `free8`
bottoming at 18,016 B during an upload - comfortably above the 12 KB floor, which made the 503s look
unexplained. The number was survivorship-biased: `free8` is read *from* `/api/state`, and
`/api/state` refuses precisely when `free8` is low, so only the samples taken when there was enough
heap to build a reply ever came back. The endpoint cannot report the heap at the moment it is too
low to report anything.

The consequence generalises to any measurement campaign against this firmware: **every heap figure
read from `/api/state` or `/api/config` is conditioned on `refuseIfLowHeap()` having passed**, so
minima and low percentiles from those bodies are biased upward, and biased hardest at exactly the
values a threshold decision turns on. Do not derive a gate threshold from them. Three sources do
not have this failure mode:

- `GET /api/debug/ui` - deliberately left outside the gate (see the comment on `refuseIfLowHeap`,
  "the small handlers are deliberately not gated ... to observe the device precisely while it is
  under pressure"). It carries `heap` and `largest_block`. Still an HTTP request, so it perturbs
  what it measures, but it does not vanish when the answer gets interesting.
- The `[net_poller]` serial heartbeat and the `[poll-heap]` trace - not requests at all, and the
  only genuinely unperturbed source.
- Recording the **status code** alongside every sample, so a refusal appears in the data as a
  refusal rather than as a missing row. That is what turned "unexplained errors" into a
  characterised behaviour in one pass.

`/api/state` keeps `heap` as `ESP.getFreeHeap()` - clients parse it, and silently changing what a
published field means is worse than an optimistic number - and gains `heap_8bit` and
`largest_block_8bit` beside it. The web app's "Heap free" tile and the device page on the panel both
show `heap_8bit`, falling back to `heap` against firmware that predates the field.

#### Could the fetch simply run at a better moment? (measured 2026-09-16, and no)

The obvious cheap lever, before reaching for an SDK rebuild: the transit poll is what eats the
heap (the GTFS-RT stream buffer, the retained updates, the TransitView and BusSchedules JSON),
while weather, Indego and alerts are small and run after it, so perhaps a TLS fetch placed at the
poll cycle's high-water mark would be admitted where the same fetch is refused 20 seconds later.
Note what is *already* true, because it bounds the idea: F12 put the transit fetch first in
`pollOnce()` years ago, so the fetch that matters most already runs at the least-allocated point
of the cycle. The only thing left to move is the optional work, from after the arrivals to before
them. That is a question about a number, so it was measured rather than guessed: `-DTRANSIT_HEAP_TRACE`
(compiled into the `cyd-*-https` envs, `net_poller.cpp`) prints one `[poll-heap]` line of 8-bit
free and largest-block at every stage of `pollOnce()`. 21 consecutive polls on the owner's board,
policy `https_preferred`, both stops live throughout:

| Stage of `pollOnce()` | free8 min | median | max | largest block |
|---|---:|---:|---:|---:|
| `poll-start` (nothing allocated yet - the best moment there is) | 38,648 | **41,200** | 41,304 | 24,564 |
| `pre-transit` | 37,020 | 39,576 | 39,680 | 24,564 |
| `post-transit` (arrivals published, decoder freed) | 34,744 | 37,040 | 42,868 | 18,420-24,564 |
| `pre-alerts` / `pre-weather` / `pre-bikes` / `pre-liveness` | 34,212 | 37,040-37,092 | 38,504 | 18,420-24,564 |
| where the gate actually ran, all 64 fetches | 27,128 | 29,748 | 42,372 | 18,420-29,684 |

The whole poll cycle is **flat to within ~6 KB**, and the best moment in it is `poll-start` at a
median 41,200 B. So the entire prize for reordering is about **11 KB** (median 41,200 against the
29,748 the gate sees today) against a **26,946 B shortfall** at that best moment (68,250 needed,
41,304 free at the very best sample). Nothing in the cycle can be moved far enough.

It is worse than that, and the second number is the one that closes the question, because it does
not depend on the gate's margins at all. Strip `kTlsHeapNeed` down to the two record buffers alone
- 33,434 B, the irreducible cost of `mbedtls_ssl_setup()`, no contexts, no peer chain, no handshake
scratch, no margin for the rest of the device - and ask whether even *that* fits at `poll-start`:
the largest block is 24,564, so the first 16,717 B buffer leaves 7,847 B in that hole, and the
41,304 B of free8 minus that 24,564 B hole leaves 16,740 B spread across every *other* hole in the
heap. One contiguous 16,717 B block would have to come out of those 16,740 scattered bytes. **Two
16,717 B buffers cannot both be allocated at the most favourable instant of the poll cycle**, let
alone the ~20 KB of contexts, kept peer chain and bignum scratch that must fit beside them. The
forced-gate run below is that arithmetic confirmed on the hardware.

The trace stays in the prototype envs: it is the measurement the SDK-rebuild question will be
re-asked with, and it costs nothing in a shipping image (`tracePoll()` compiles to an empty inline
function without the flag). Moving optional work ahead of the arrivals was never free either - F12
exists precisely because a weather refresh between SEPTA's answer and the screen made arrivals
12 s old - so the reorder would have cost the thing the device is for, to gain 11 KB of the 27 KB
missing. It was not made.

#### Design decision, as measured

Of the four shapes considered - (i) prefer HTTPS, fall back to HTTP by heap; (ii) HTTPS required,
report the stop unavailable otherwise; (iii) HTTPS for the small JSON fetches only; (iv) not feasible
without changing something else - **the prototype implements (i) with (ii) as a runtime setting of
the same code, and the measurement says the answer today is (iv)**:

- The gate design is right and stays. It asks the allocator the exact question ("can I have two
  16,717 B blocks right now, with 6 KB contiguous beside them and 68 KB free in all?") by allocating
  and freeing them (`tlsAffordable()` in `http_fetch.cpp`), before any connection is opened, so
  **the transport is chosen by the device's memory, never by the network**. On this board it says
  no every time, and under `https_preferred` the fetch then goes over plain HTTP, counted and shown
  (`transport.http_by_heap`, the "Data link" tile). Arrivals were unaffected for the whole test.
- **Fail closed, tightened.** A TLS attempt that fails - certificate, handshake, timeout, *or the
  heap running out inside mbedTLS after the gate said yes* - is reported as unreachable and is not
  retried over HTTP. The prototype's first draft made one exception, "an allocation failure inside
  mbedTLS is the answer the gate would have given a millisecond earlier, so continue over plain
  http"; it was removed because the same error codes come out of mbedTLS while it parses the
  *peer's* certificate chain (`MBEDTLS_ERR_X509_ALLOC_FAILED` for an oversized chain,
  `SSL_ALLOC_FAILED` for a handshake message it cannot buffer), so whoever answers the connection
  could have forced the downgrade. Once the gate has chosen TLS for a fetch, that fetch is TLS or
  nothing; the next poll asks the gate again.
- (iii) does not help: the record buffers are the same size for a 300-byte weather reply as for the
  150 KB feed.
- (ii) costs nothing extra and is there for an owner who would rather see "unavailable" than an
  unverified time - on this board that is "unavailable" always, which the setting's own label says.
- (iv), then, is the honest answer: what stands between this board and verified HTTPS is not flash
  (18 KB net, affordable) and not the gate, but ~27 KB of byte-addressable heap at the best moment
  of the poll cycle, which no ordering of the fetches can find.

**Shipping decision.** HTTPS does **not** go into the shipping envs. The three conditions the owner
set were: flash headroom on `cyd-3248S035R` stays at ~20 KB or better at `CORE_DEBUG_LEVEL=0`,
steady-state heap with TLS in use stays within §12.1's exception-safety posture, and arrivals get no
slower or less reliable. Measured:

- **Flash: passes, with a caveat.** 41,046 B of headroom in the 3.5" prototype against 59,154 B in
  the shipping env. But the six board envs are not alike: `cyd-3248S035C` already ships at
  1,860,586 B (39,958 B headroom), so adding TLS to *every* env would leave it at about 22 KB -
  inside the letter of the rule and uncomfortably close to the partition, on a board nobody has
  measured HTTPS on.
- **Heap: fails, in the strongest possible way.** Not "the margin is thin" but "the session never
  opens": 0 admissions in 64 fetches at the shipping gate, 1 in 54 at a gate stripped to the two
  record buffers, and that one died with `MBEDTLS_ERR_SSL_ALLOC_FAILED`. The §12.1 clause can never
  even be evaluated, because there is no steady state with TLS in use to measure. On the day a
  rebuilt SDK makes sessions open, this is the clause to re-measure first: §12.1's invariant is that
  every allocating task can *catch* `std::bad_alloc`, and a live TLS session parks 42 KB in the same
  heap the poller, the web handlers and the LVGL loop allocate from.
- **Arrivals: unchanged, which is the one good result.** Over the 21-poll soak both stops stayed
  `live`, `last_poll.ok` never went false, and the cadence matched the shipping build - because the
  gate runs before any connection and costs two mallocs. The prototype is safe to run; it just never
  does anything.

So shipping it would cost ~18 KB of flash on every board, 2 KB of poller stack and the core's error
lines (and on `cyd-3248S035C` most of the remaining headroom), to deliver a feature that reports "Plain HTTP this time (not enough free memory)" on every
poll. The prototype envs stay (`cyd-3248S035R-https`, `cyd-2432S024C-https`), `device.transport`
defaults to `http` in every build, and **`https_preferred` is the documented opt-in**: build a
`-https` env, flash it, set *Settings → Data & weather → Data connection* to "Encrypted when memory
allows", and read the "Data link" tile on the Now page. On today's hardware it will read "Plain
HTTP this time"; on a board with the rebuilt SDK below it is the switch that turns HTTPS on.

**What it would take**, in order of leverage:

1. Rebuild `framework-arduinoespressif32-libs` (pioarduino's `esp32-arduino-lib-builder`) with
   `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`, `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048` (a client
   sends tiny requests), `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` and `CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y`
   (free the input buffer and the peer chain between records), `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n`,
   `CONFIG_MBEDTLS_HAVE_TIME_DATE=y` (so an expired certificate fails closed, which the review asked
   for and this SDK cannot do), `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=n`. That is 16.7 + 2.3 KB of buffers
   plus ~4 KB of contexts - **~23 KB per session** with dates checked. Against 27-42 KB free at the
   fetch point with a 13-37 KB largest block, that fits *often*, not always; the gate then does what
   it was built for. The input buffer cannot go below 16 KB for SEPTA: only Open-Meteo honours
   `max_fragment_length`. It is a build project of its own (a custom `platform_packages` entry,
   reproducibility, keeping it in step with the pinned core), which is why it is a recommendation.
2. Give the fetch point more 8-bit heap. The candidates are all trade-offs the owner would feel: the
   LVGL pool (36 KB static, 74 % used), the 19 KB draw buffer (1/20 of the screen already), the
   18 KB the SD mount takes, the arrival tracker (8 trips per stop). 10-15 KB is realistic; 35 KB is
   not without dropping a feature.
3. A board with PSRAM (the ESP32-S3 Sunton variants) makes the whole question go away; the
   `platformio.ini` envs are per board already.

#### What was verified on the owner's board (2026-09-16, `cyd-3248S035R`, firmware built from this branch)

Flashed with the OTA path first (`-H "Expect:"` needed, README) and over serial after the OTA gate
started refusing (above); serial console captured throughout; the owner's config (stops 21332 and
21297) untouched, verified after every flash.

- **Boot and stack.** `[heap] eh_pool arena=2048` (the `next` exception pool is in the image).
  Poller stack high-water mark on the plain build: **4,556 B free of 10 KB**, which is the 3-4 KB a
  TLS handshake needs on the stack and nothing over; the HTTPS builds give the poller 12 KB
  (`net_poller.cpp`; 6,536 B free measured with plain fetches over 21 polls). Static RAM +448 B.
- **`https_preferred` soak.** 21 consecutive polls (`cyd-3248S035R-https`, flashed over serial,
  owner's config, both stops `17-21332` and `17-21297` `live` for the whole run, every
  `last_poll.ok` true). 64 fetches asked for `https://`; the gate admitted **none** of them:
  `https_ok` 0, `https_failed` 0, `cert_failed` 0, `refused_by_heap` 0, `http_by_heap` 68 (the
  four extra are the boot-time BusSchedules retries before the first `poll-start` line). The gate
  saw a median 29,748 B of 8-bit heap with a 19,444 B largest block against a need of 68,250 -
  never closer than 25,878 B. `/api/state`'s `transport` block reported it live throughout
  (`"last":"http"`, `gate_free` 29,304, `gate_largest` 19,444 at the end of the run), and the
  web app's Now page read "Data link - Plain HTTP this time (not enough free memory for HTTPS)".
  Arrival latency, poll cadence and stop health were indistinguishable from the shipping build.
- **Poll-cycle heap trace** (`-DTRANSIT_HEAP_TRACE`, same run): the `[poll-heap]` table under
  "Could the fetch simply run at a better moment?" above. The short version is that the cycle is
  flat to within 6 KB, its best moment is 41,200 B of 8-bit heap with a 24,564 B largest block,
  and two 16,717 B record buffers do not fit there even with every other TLS cost set to zero.
- **Policy tests (device suite section H, added tonight; 9/9 passed).** The `transport` block is
  well formed and `GET /api/config` echoes the policy. `https_preferred`: fetches kept being decided
  (all refused by the gate), `refused_by_heap` never moved, and `last_poll.ok` stayed true with both
  stops `live`. `http`: only `http_by_policy` moved across three polls and the TLS counters froze.
  `https`: across three polls `http_by_heap` and `http_by_policy` did not move while
  `refused_by_heap` climbed - every fetch was refused rather than sent in clear, and the stops read
  unavailable, as the setting promises. `"transport": "bogus"` is a 400 at `device.transport`. The
  owner's setting was restored afterwards. In the same run, section A's `POST /api/debug/oom` (the
  `next` exception-pool proof) aborted the device instead of answering `caught:true` - see the note
  after this list; it is independent of HTTPS.
- **Forced gate** (`PLATFORMIO_BUILD_FLAGS=-DTRANSIT_HTTPS_GATE_TEST pio run -e cyd-3248S035R-https`
  - the env var appends to `build_flags`, `BOARD_NAME` and the rest survive, checked in the
  `https_env.py` line of the build log). The gate is stripped to the irreducible cost: `kTlsHeapNeed`
  = 33,434 (the two record buffers, nothing else) and no contiguity margin; `/api/state` confirmed
  `"heap_need": 33434`. Result: across the run 54 fetches asked for `https://` and the gate admitted
  **exactly one** of them, the very first one after boot, at the cleanest heap the device ever has
  (`poll-start` free8 49,852, largest 47,092 - a good 8 KB better than any steady-state moment; the
  other 53 were refused), and **mbedTLS still could not open the
  session**: all three attempts on SEPTA's TripUpdates failed in 38-76 ms with
  `code -0x7f00` = `MBEDTLS_ERR_SSL_ALLOC_FAILED`, free8 34,264-34,920 at the failure. So the
  ~20 KB of contexts, kept peer chain and handshake scratch beside the buffers is real, and 49 KB
  of 8-bit heap with a 47 KB hole in it is *still* not enough for one session on this SDK.
  **It failed closed**, which is the behaviour this section promises: the three attempts stayed on
  TLS, the fetch was reported unreachable, `https_failed` went to 1 with `cert_failed` 0 and
  `last_tls_error` -32512 while `http_by_heap` did not move for it, and not one byte of that URL
  left the device over plain HTTP. The stops were back to `live` on the next poll. Switching
  `device.transport` to `http` at the end of the run froze the TLS counters and moved only
  `http_by_policy` (26 fetches), on the live device, which is section H's `http` case observed
  outside the suite.
  Every *later* fetch was refused even at the relaxed need, and one of them is the whole argument in
  a single line: `TransitView … heap free=37076 largest=32756, need 33434`. Both scalar tests pass
  (37,076 > 33,434; 32,756 > 16,717) and the gate still said no, because it does not trust scalars -
  it asks the allocator. 32,756 - 16,717 leaves 16,039 B in that hole and only 4,320 B lies anywhere
  else, so the second 16,717 B buffer has nowhere to go. That is the arithmetic of "Could the fetch
  simply run at a better moment?" happening on the hardware.
  One caveat to record honestly: the poller's stack high-water mark through this run was 6,328 B
  free of 12 KB, only ~200 B worse than the plain-fetch soak, because the session died inside
  `mbedtls_ssl_setup()` before any bignum work. **The 3-4 KB of handshake stack estimated above has
  therefore never been exercised on this board** - the 12 KB stack the `-https` envs give the poller
  is still a paper number, and the SDK-rebuild test pass has to re-measure it. No watchdog trip, no
  `Guru`, no reset in either run.
- **Not testable on this hardware:** a certificate that fails to chain to its pin (test hook
  `-DTRANSIT_HTTPS_BADPIN_TEST` in `tls_roots.h`, SEPTA pinned to ISRG Root X1) and the handshake
  time per host, because no handshake completes before the heap runs out. Both are what the rebuilt
  SDK's test pass must cover first: expected `[https] … code -0x2700`, `transport.cert_failed` = 1,
  `transport.last` = `https_failed`, the stop unavailable, and no plain-HTTP request for that URL
  leaving the device (`tcpdump port 80` on the router).

**Seen in passing, not HTTPS: the exception-pool proof can still abort.** On a fresh boot the
first `POST /api/debug/oom` ended in `abort()` on core 1 - `__cxa_throw` → `__cxa_get_globals`
(`eh_globals.cc:150`) → `std::terminate`. Decoded against the ELF and confirmed in the disassembly:
a thread's *first* throw calls `pthread_getspecific`, finds nothing, `malloc`s its
`__cxa_eh_globals` and terminates if that fails, and the endpoint exhausts the heap by design
before throwing. The 2 KB emergency pool supplies the exception object, not this one-time
per-thread block, so the proof passes only on a task that has thrown before (the full device
suite provokes caught `bad_alloc`s on the async task in its earlier sections; a run that hits the
endpoint first does not). The fix - warming every throwing task's exception globals at start, while the
heap is plentiful - landed on 2026-09-16 as `warmExceptionGlobals()`; §12.1 has it. Consistent with that diagnosis:
the same endpoint, hit as the *fourth* check of the 2026-09-16 suite run on a device that had been
up 75 s and polling, answered `caught:true` with a 12 B largest block at the throw. It is not
flaky - it depends entirely on whether that task has thrown before, which is exactly the gap.

The serial lines to read: `[https] plain http for <url>: heap free=… largest=…, need …` on every gate
refusal (the numbers are 8-bit heap), `[https] <url>: 200 over TLS, headers in N ms; heap free=…
largest=… min_free=…` on a success, `[https] <url> attempt i/3: no response over TLS after N ms,
code -0x…` on a failure, `[https] refused …` under policy `https`, and `[net_poller] free_heap=…
free8=… largest_block=… stack_free=…` once per poll.

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
the browser to offer the stations nearest each configured stop. Cadence 10 min (5 min until
0.3.1; halved because each refresh is the single most expensive thing a poll cycle does - the
measured per-stage ring puts the bikes stage at -17.6 KB of byte-addressable free heap with the
largest free block down to 6.4 KB, held to the end of the cycle - and dock counts do not move
meaningfully in ten minutes. The first fetch of a boot and any fetch forced by a config change are
unaffected.)

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
    host_match.h                 the DNS-rebinding Host check as pure, non-allocating string work
                                 (§12.1; it runs where a throw would be fatal), host-tested
    json_response.h              sendJsonStreamed(): the one way a large JSON body leaves the
                                 device, used from both the web task and the poller task (§12.1)
    cpu_yield.{h,cpp}            how a CPU-bound loop on the poller lets IDLE0 run, and the
                                 high-water it reports as cpu_stretch_ms_max (§12.1)
    ui/                          LVGL screens: main, stats, device info; uses only Snapshot data
    ui_lock.{h,cpp}              who may wait on whom: the display task's zero-wait lock policy and
                                 the last-good fallback every shared accessor uses (this §)
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

**Nor may it wait on ANY other task's mutex. Not briefly: NOT AT ALL** (2026-09-16). The display
task's budget for waiting on a lock another task can hold is zero ticks, and "a short cap" is not a
weaker form of that rule - it is a different rule, and the wrong one (below). This had been kept at
each call site, which is not a mechanism: a pass in the RC review capped
`getStopSummary()` and `tryGetPollStatus()` at 50 ms and left every other accessor on its original
500-1000 ms wait, and one of those - `getBikes()`, 500 ms, called from `refreshMainScreen()` on
every tick the arrivals page is up - panicked a device in `vTaskPriorityDisinheritAfterTimeout`
(§12.1, second occurrence). The decision now lives in one place, `src/app/ui_lock.h`: every shared
accessor takes its lock through `takeShared()`, which picks the wait from **who is asking** rather
than from what the call site remembered to pass - a zero-tick try on the display task, the
accessor's own wait on every other.

**The two accessors that already looked fixed were not the pattern to copy** - they were a narrower
version of the same bug, and both are now zero-budget like everything else. Zero and not "short",
because a short wait does not close the hole. That assert is reachable on
exactly one path: a take that blocks, times out, and donated priority on the way in. A shorter
timeout makes a blocked take time out *more* often, not less - it narrows the window instead of
closing it. With `xTicksToWait == 0` the take returns without ever blocking, so it never donates
priority and FreeRTOS says so itself on that branch (`configASSERT( xInheritanceOccurred ==
pdFALSE )`): the assert is unreachable rather than unlikely. The same fact removes the rest of the
problem for free - a task that never blocks on a lock cannot be priority-inverted behind whoever
holds it.

A miss costs one frame, never a blank panel. Each accessor keeps a `LastGood<T>` of the value it
last handed *this* task and returns that when the lock was busy: `snapshotPtr()`, `getBikes()`,
`getWeather()`, `headerWeatherText()`/`Temp()`/`Icon()`, `stopWeatherNote()` and `getStopSummary()`
(both keyed per stop), `getSdStatus()`, `tryGetPollStatus()` and `tryGetActiveConfig()`. Those
holders are only ever touched when the caller is the display task, so they are per-task state that
happens to be spelled `static`, and they need no lock of their own. Every other task keeps its
blocking wait, which is right: the web task is answering one request, a few milliseconds cost it
nothing, and a stale field would be a worse answer than a short pause.

**The writers were the other half**, and fixing them is what keeps misses rare rather than routine.
A reader's wait is only as long as a writer's hold. `publishSnapshot()` used to copy the whole
Snapshot - vectors of arrivals, a string per field - *with the mutex held*, so a reader was not
waiting for a pointer, it was waiting out somebody else's allocation. The Snapshot is now published
as a `shared_ptr<const Snapshot>` built outside the lock and swapped in, weather and bike publish by
swap so the outgoing value is freed outside too, and what happens under any of these mutexes on the
display path is a refcount bump or a pointer exchange. Two things fall out of that beyond the
latency: the display task no longer copies the Snapshot at all - it holds a reference to the
poller's own, which is one whole Snapshot less live heap and one less allocate/free cycle per second
fragmenting it - and `GET /api/state` borrows it the same way instead of copying it, which is a
whole Snapshot less peak heap per request on a device whose largest free block sits at 5-25 KB
(§12.1).

**And it is observable, not just asserted.** `GET /api/debug/ui` reports `lock_misses` (reads that
found a lock busy and redrew last frame's value) and `tick_ms`/`tick_ms_max` (how long a refresh
took, worst since boot). A tick is single-digit milliseconds; a reading in the hundreds is what a
display task that waited for something looks like, and the only things it can wait for are the locks
it is not allowed to wait for. That is the check a future screen would fail.

**What the audit found and deliberately did NOT change**, so the next reader does not have to
rediscover it: the display path still calls into the Wi-Fi driver. `WiFi.status()`, `WiFi.RSSI()`,
`WiFi.SSID()` and `WiFi.localIP()` are read from `refreshDeviceInfoScreen()`, and
`refreshMainScreen()` reads `status()`/`RSSI()` for the header's signal bars on every 1 Hz refresh
(`main_screen.cpp`, twice each - a free cleanup for whoever is next in that function). `RSSI()`
reaches `esp_wifi_sta_get_ap_info()` and takes the SDK's Wi-Fi API lock, so this is, strictly, the
display task waiting on a lock another task can hold.

It is left alone on purpose, and the reason is the same fact that makes the rest of this section
work: that lock is taken with `portMAX_DELAY`. A take that never times out can never reach
`vTaskPriorityDisinheritAfterTimeout()` - the assert needs a *timeout* - so these calls cannot
produce the panic in §12.1, first or second occurrence. What they can cost is latency, and that is
measured rather than assumed: `tick_ms_max` sat at 168 ms across a full device-suite run whose page
rebuilds are the expensive part, so they are not costing anything now. Caching RSSI off the poller
would remove the theoretical stall and add a staleness question to a number that is already only a
four-bar icon; if it is ever done, do it for a measurement, not for this paragraph.

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

Memory rules: no full framebuffer; the LVGL partial buffer is **1/30** of the screen in RGB565 on
the 3.5" boards and 1/16 on the 240-tall ones (the library default of 1/4 with 3-byte pixels does
not fit; `firmware/boards/README.md` has the history, including 1/20 → 1/30 in 0.3.1 for 5,120 B of
heap — this line said "1/10" until then, which was never any board's value); large long-lived
objects (`ArrivalTracker` **8,040 B** and `StatsAggregator` **8,744 B**, both measured on the target
ABI rather than the "~16 KB / ~8 KB" this line used to carry) are heap-allocated, never file-scope
globals, because the ESP32's static .bss budget is separate from and much smaller than the heap; one
TLS connection at a time; ArduinoJson documents sized from measured payloads (§4) with 25 %
headroom; log free heap once per poll at `INFO`; refuse to start OTA below 16 KB of `MALLOC_CAP_8BIT`
free with a 5,876 B largest block (§2.1 - the byte-addressable heap, not `ESP.getFreeHeap()`).

**The poll working set: one shared buffer, lent to each stage in turn (0.3.1).** This is the same
rule as the line above, applied to the things that were NOT long-lived and should have been — and it
took two attempts to get right, which is worth recording because the first one was reasonable and
wrong.

A cycle used to build its own buffers every time: a `GtfsRtStream` (a 4,096 B entity buffer and a
~4,600 B retention block), a `std::vector<uint8_t>` body grown from nothing by doubling for each
TransitView / BusSchedules / Alerts / Arrivals response, a 24 × 128 B `SchedEntry` block per
schedule parse, the transport's own 4 KB BusSchedules buffer, and a 6,144 B Indego feature buffer.
Every one is a request for a **contiguous** block, and they all land within a few hundred
milliseconds of each other. Free heap is not the constraint on this board; the largest free *block*
is (§12.1). It rests at 25–28 KB and decays, and at **11.7 KB — with 36 KB still free —** the cycle
threw `std::bad_alloc`, reported every stop as "out of memory during fetch", and every cycle after
it did the same until the heap-wedge self-heal rebooted the device.

**The first attempt gave each of them a permanent buffer, and that was a net loss.** Measured on the
owner's board (0.3.1-rc1, five minutes, sampler at 15 s):

| | diag1 | rc1 |
|---|---:|---:|
| per-cycle contiguous demand | ~12 KB | **~5 KB** |
| resting free8 | ~35 KB | 22 KB |
| resting largest block | ~24.5 KB | ~14 KB |
| **margin (largest − demand)** | **~12 KB** | **~9 KB** |

The demand halved and the margin still got *worse*, because holding ~26 KB permanently is how the
floor drops. And `min_free8` reached **696 B at 115 s of uptime** — before any Indego refresh —
from a combination that no stamped stage can see: a poll mid-fetch (~15–17 KB) plus one concurrent
`/api/state` on the AsyncTCP task (~5 KB document plus response copies) plus the Wi-Fi/lwIP receive
burst. On diag1's 35 KB floor the same combination bottomed at 1,452 B. A trough to zero is an lwIP
`assert`, not a caught `bad_alloc` (§12.1). **So the floor matters as much as the per-cycle demand,
and a fix that trades one for the other is not a fix.**

**What rc2 does instead.** Those buffers are never live at the same time. They are all "one thing at
a time" byte buffers, on one task, in sequence — the entity buffer during the TripUpdates fetch
(dead the moment `finish()` is called), then each buffered JSON response, then the Indego scanner's
one-feature buffer — so **one 6,144 B vector, sized for the largest borrower, is handed from stage
to stage**:

- `GtfsRtStream::setEntityBuffer()` and `indego::StatusStream::setFeatureBuffer()` borrow a
  `std::vector<uint8_t>*` — the container, not its storage, so a reallocation by a larger borrower
  cannot dangle the borrow — and each gives back what it was holding when it takes one.
- `pollBusStops()` hands the borrow back immediately after `fetchRealtimeEx()`, before the first
  buffered response needs it.
- `PollBuffers::beginCycle()` releases growth beyond the reservation at poll-start, so one oversized
  response cannot become resident. That is the floor drift above, in miniature.

One buffer stays separate: the **transport-level BusSchedules body** (`net_poller.cpp`), because
that layer buffers the response before `transit_core` sees it and then hands it over, so for that
moment two really are live at once. It also sits on the path implicated in the collapse — the
four-attempt loop inside `fetchPlausibleSchedule()`'s three, re-entered every two minutes while
SEPTA answers with the wrong service day — so it keeps a 4 KB reservation of its own.

rc2 left two things deliberately **per-cycle**: the GTFS-RT retention block and the BusSchedules
parse block, both `std::vector`s of non-trivially-destructible values that cannot share raw bytes
without a custom allocator. At a ~24 KB resting largest block a 4.9 KB request is one the heap
carries — and on 2026-09-17 the heap stopped carrying it.

**The typed blocks, and why rc2's judgement was wrong (0.3.2-rc1).** v0.3.1 ran healthily for 35–40
minutes on the owner's board (resting free8 39–40.7 KB, largest ≥22.5 KB at every poll start) and
then fragmented to free8 17–20 KB with a **3,444 B** largest block. From that point every cycle
failed in the same place: the ring read `pre-transit → oom-transit` with **no stage in between**,
which names the allocation exactly — the first contiguous request of the rt-stream stage, which with
the entity buffer already borrowed is `retainUpdates()`'s `reserved_.reserve(32)`. On-target sizes,
read out of the image's DWARF rather than estimated: `sizeof(StopTimeUpdate)` **152 B**,
`sizeof(TvVehicle)` **176 B**, `sizeof(SchedEntry)` **128 B**. So that reservation is **4,864 B**,
against a 3,444 B largest block, every 30 seconds, forever.

Both typed blocks are now **resident vectors in `PollBuffers`, lent out** the same way the byte
scratch is — `retained` through `GtfsRtStream::setRetentionBuffer()`, `tv` as the vector
`fetchTransitViewAppendEx()` parses straight into:

- **The retention block is sized from the config**, not from the 32-slot default cap: 8 slots per
  configured bus/trolley (stop, route) pair, which is exactly what the stream's per-pair cap allows
  a pair to hold, so nothing is lost. The owner's two Route 17 stops need 16 slots = **2,432 B**
  instead of 4,864 B.
- **The TransitView list is one vector for the whole cycle.** It used to be two — the parser grew
  its own from `reserve(8)` by doubling, and `tv_all` grew again on `insert()` — so a rush-hour
  Route 17 (20–30 vehicles) asked for a 5,632 B contiguous block *twice*, with the retention block
  live. It is reserved once at `kMaxTvVehicles` = **5,632 B** and every route appends into it.
  `refreshRouteLiveness()` in the optional tail borrows the same vector rather than taking a local.
- **The BusSchedules parse block stays per-cycle** at 24 × 128 = **3,072 B**, because it is asked
  for only on a schedule *refetch* — once per stop per 10 minutes, not every cycle — and making it
  resident would cost the floor 3 KB to speed up a path that already succeeds.

**What this costs the resting floor: 8,064 B** for the owner's config (2,432 + 5,632), which is the
whole of the added residency. rc1's lesson is not repealed by this, it is respected: the same trade
is being made, but for 8 KB instead of 26 KB, and against a failure that was actually measured
rather than predicted.

Resident total: **19,328 B** (6,144 shared scratch + 4,096 transport buffer + the 1 KB error-reply
reserve of §12.1 + these 8,064). Measured on the host with the allocation probe, a warm cache-hit
cycle now makes **no** request of 2 KB or more at all; the largest single contiguous request left in
such a cycle is an arrival vector at ~1.2 KB, and in a schedule-refetch cycle the 3,072 B parse
block. `min_free8` on `GET /api/debug/ui` is the number that says whether the floor is right, and it
is the first thing to read after flashing.

**And the shared scratch no longer grows and shrinks.** `kJsonBodyCap` is 16 KB but the reservation
is 6,144 B, so a body larger than the reservation made the vector *double* past it — a 12,288 B
contiguous request — and `beginCycle()` then handed that block back and took a fresh 6,144 B one,
every cycle, for as long as that endpoint kept answering big. Grow, shrink, grow, shrink: exactly
the churn this section exists to remove, and invisible because nothing recorded how big a body had
ever been. Now `fetchBuffered()` reserves the rounded-up size in **one** step (no doubling), and the
reservation **ratchets** up to it and stays there — one reallocation per boot instead of one per
cycle — bounded by `kScratchMaxReserve` (10 KB) so a pathological response still cannot become
resident. `scratch_max_bytes`, `scratch_reserve_bytes` and `scratch_grows` on `GET /api/debug/ui`
are the measurement that says whether `kScratchReserve` should simply be a different number in the
source, which is better than either behaviour and which this firmware previously could not answer.

Two consequences follow the same rule and are worth stating where a reader will look for them.
`publishSnapshot()` takes its Snapshot **by rvalue and returns the published pointer**, so the
poller holds one Snapshot and not two through the optional tail (§12.1's `snapshots_live` says so:
1 between publishes, 2 for up to a second after one). And `mountSd()` runs **before**
`startWebServer()`, because the FAT mount is one ~12.5 KB contiguous `calloc` and should come off a
heap the async web server has not been allocating out of yet.

Flash budget: the app slot is 1,900,544 bytes. As of 2026-09-16, with the §12 hardening, the screen pass, the LVGL pool safety work, the release-candidate fixes and the display task's lock policy (this §), the full feature set uses **1,862,942 B (98.0 %)** on `cyd-3248S035R` — 37,602 B of headroom — and 1,858,614 B (97.8 %) on `cyd-2432S024C`; the tightest env of all is the HTTPS prototype `cyd-3248S035R-https` (§2.1), which ships in no image. (This line read "93.9 % / 93.7 %, ~112 KB headroom" until 2026-09-16, which was the 2026-09-15 measurement left behind by three later passes — the same failure §2.1's threshold note describes, so the figures here are now absolute bytes with the date they were taken.) `firmware/README.md` carries the per-env table and ranks what to cut if more is needed — the largest single item is the setup screen's QR code at 17 KB. Do not grow the app slots without dropping OTA.

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
    "night": { "enabled": true, "after_min": 60 },
    "nightly_restart": { "enabled": true, "time": "03:30" }
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

`device.transport` (`http` | `https_preferred` | `https`, default `http`) is read, written and
honoured only by a firmware built with `-DTRANSIT_HTTPS` (§2.1); a shipping build ignores the key
exactly as it ignores v0.1.x's `use_https`/`tls_verify`, never emits it, and fetches everything over
plain HTTP. In an HTTPS build `https_preferred` asks for verified TLS on every fetch and uses plain
HTTP only when the heap gate says a session does not fit right now (never after a TLS failure),
`https` uses TLS or reports the fetch unreachable, and `PUT /api/config` changes the policy for the
very next fetch. The web app shows the field only when `/api/state` carries a `transport` block.

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
- Clock strings: `quiet.start`, `quiet.end`, `nightly_restart.time` and every `profiles[].start` /
  `profiles[].end` are local `"HH:MM"`, 24-hour, validated the same way and parsed by the same
  `daypart::parseClock()`.
- `device.nightly_restart` (0.3.2-rc1) is `{ "enabled": true, "time": "03:30" }` and is **on by
  default**. A config saved by older firmware carries no such block and gets exactly those
  defaults — that is the intended behaviour for an existing device, not a migration, and the web
  form's fallbacks match it so it cannot show as off while the device has it on. §12.1 explains
  what it is for and, as importantly, what it is not.
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
| 421 | `{"error":"this device is not reachable under that host name"}` | The `Host` header is not the device's IP, `<device name>` or `<device name>.local`, `192.168.4.1` or `localhost` (optional `:port`, case-insensitive). DNS-rebinding defence — checked at **end of headers**, on every route, by a first-registered `AsyncWebHandler` whose `canHandle()` claims the request when the `Host` is not one of ours (`HostGuardHandler`, §12.1). That is before the body is parsed and therefore before any body or upload callback, so `POST /api/ota` needs no repeat of the check and a rebound upload never reaches `Update.begin()`. It used to be a server middleware, which runs *after* the body and could reboot the device; §12.1 has why. A bad-`Host` request with a body has that body read and discarded — counted, never parsed — before the 421 |
| 409 | `{"error":"another firmware upload is in progress"}` | A second `POST /api/ota` while one is streaming |
| 500 | `{"error":"..."}` | `PUT /api/config` could not write the file (the live config is unchanged, §6) |

`GET /` additionally answers with `X-Frame-Options: DENY`, `Content-Security-Policy:
frame-ancestors 'none'` and `X-Content-Type-Options: nosniff`. There is no `script-src` directive:
the Stops page loads Leaflet from a CDN (§10).

| Method, path | Purpose |
|---|---|
| `GET /` , `/app.js`, `/app.css`, `/favicon.svg` | Web UI, served gzip with `Cache-Control: max-age=3600`, ETag = firmware build id |
| `GET /api/state` | Current snapshot: time, uptime, heap, wifi {ssid, rssi, ip, mdns}, sd {mounted, free_mb, log_bytes, dropped_rows, write_ok, error} (§9.1), last_poll {ok, age_s, error, since_s}, `stops[]` each with `arrivals[]` (§8 shape), `ok`, `health`, `source_ts`, `source_age_s` (-1 when the feed carried no timestamp) and `weather_note`, `alerts[]`, `weather {enabled, units, age_s, stale, main {temp, feels_like, code, text, wind, hours[]}}` (§4.8; `age_s` is the main location's last successful fetch, `stale` once that is over an hour old), plus `board` (the PlatformIO env this image was built for, e.g. `cyd-3248S035R`), `auth {pin_required}`, `config_recovered` (§6) and `last_restart {esp, reason, detail, uptime_s}` (§12.1). `last_poll.since_s` is seconds since a poll cycle last *completed* — success or failure — and is `-1` until the first cycle of this boot; it is deliberately not `age_s`, which only moves when a poll *reports*, so a poller stuck inside a fetch freezes `age_s` while `since_s` keeps climbing. `last_restart.esp` is `esp_reset_reason()` (1 power-on, 3 software restart, 4 panic, …); `reason` is `poll_stall`, `heap_wedge`, `lvgl_pool` or `""`, and names only the restarts this firmware asked for itself; `detail` is the one-line plain-English version with the numbers that caused it. The note lives in RTC memory, survives `ESP.restart()` but not a power cycle, and is reported for exactly one boot. A firmware built with `-DTRANSIT_HTTPS` (§2.1) adds `transport {policy, last, https_ok, https_failed, cert_failed, http_by_heap, http_by_policy, refused_by_heap, last_tls_error, last_https_ms, heap_need, gate_free, gate_largest}`: `last` is the transport of the most recent fetch that asked for `https://` (`https`, `http`, `https_failed`, `refused`, or `none`), the counters are per boot, and `gate_*` are the 8-bit heap numbers of the last gate decision against `heap_need`. Absent from shipping builds |
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
| `GET /api/debug/ui` | Test hook (not for the web UI; open, read-only): current page (main/night/stats/device, or `stalled`), dimmed + applied brightness, due/chime counters, active profile, shown stops, hidden alternative panels, ticker text, header weather, resolution, heap, and the LVGL pool: `lv_used`/`lv_free`, `lv_max_used` (high-water since boot), `lv_total` (= `LV_MEM_SIZE`; used + free falls a little short of it, the difference being TLSF's per-block overhead), `lv_frag_pct`, `lv_page_cost` per page, `lv_page_refusals`, and `lv_tight` when the page that is up left under ~3 KB. Also `lock_misses`, `tick_ms`/`tick_ms_max` (§5) and `cpu_stretch_ms_max` — the longest the poller task has run a CPU-bound loop without letting IDLE0 in, since boot, against the task watchdog's 5,000 ms (§12.1). Since 0.3.1 also `heap_8bit` and `min_free8` (the byte-addressable free heap now, and the lowest it has ever been — §2.1), `snapshots_live`, `failed_polls`, `wedged_polls`, `proxy_queue_depth`, `stack_hwm` per task, and `oom_replies_dropped` / `heap_reserve_held` (§12.1, "the 503 for out of memory needs memory"), and `bt_release_rc` / `bt_release_gain_bytes` — `esp_bt_mem_release()`'s return code and the `MALLOC_CAP_8BIT` free-heap delta across it, reported here because the serial console cannot be captured on the owner's bench — and `in_flight_requests` / `admission_refusals` / `max_in_flight_requests` (§12.1, accept-time admission control). Since 0.3.2-rc1 also `scratch_max_bytes` / `scratch_reserve_bytes` / `scratch_capacity` / `scratch_grows` — the largest response body ever buffered, what the shared poll scratch is reserved at, and how often a body went past it (§5); `heap8_info` (`heap_caps_get_info(MALLOC_CAP_8BIT)`: `total_blocks`/`free_blocks`/`allocated_blocks`/`largest_free_block`/`minimum_free_bytes` — 40 KB free in one piece and 40 KB free in thirty pieces read identically on every other line here, and this is the difference); `sched_cache_bytes` / `alerts_cache_bytes` / `snapshot_bytes` / `retained_slots` / `tv_slots`; and `?log=1` for the 240-row per-cycle memory log (`cycle_log`, `cycle_log_first`, `cycle_log_rows`, `cycle_log_seq`, `cycle_log_cap`), which returns the log INSTEAD of `trace` because the two share a render buffer. Each log row is `[uptime_s, free8, largest, min_free8, flags]` at poll-start, flags being 1 schedule refetch, 2 alerts fetch, 4 weather, 8 bikes, 16 out of memory, 32 poll failed, 64 clock unsynced. Also `oom_streak` / `wedge_reason` / `oom_polls_before_reboot` (§12.1, the self-heal tallies) |
| `POST /api/debug/tap` | Test hook (PIN-protected: it changes what the screen shows): simulated touch (press + click on the LVGL task), so page cycling and quiet-hours wake can be exercised without the panel |
| `POST /api/debug/page` | Test hook (PIN-protected, same reason): go straight to `main` \| `night` \| `stats` \| `device`, named in `?page=`, a `page=` form field or the raw body. Performs exactly the transition a tap does, queued for the LVGL task like `/api/debug/tap` — nothing builds an `lv_obj` on the web server task. It exists because LVGL pool exhaustion cannot be reproduced in the simulator's 512 KB pool (§8) and measuring it wants thirty cycles, not thirty taps |
| `POST /api/debug/oom` | Test hook (PIN-protected: it starves every other task for under a millisecond): exhausts the heap on purpose, forces a `std::bad_alloc`, frees everything and answers `{"caught":true,"blocks":N,"largest":B,"free_before":X,"free_after":Y}` - the deterministic proof of the emergency exception pool (§12.1). `largest` under ~100 means the exception object could only have come from the pool; without the pool the request reboots the device. Run it on a **fresh boot**: it then also proves the first-throw path (§12.1), which on a task that has already thrown answers `caught:true` either way |

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
  numbers around.
- **Exactly one page is resident at a time.** Every transition parks on a permanent empty screen,
  deletes the page it is leaving, and only then builds the next one; the night page replaces the
  arrivals page rather than sitting beside it. This is a correctness rule, not an optimisation.
  LVGL draws every widget from a fixed 36 KB static pool (`LV_MEM_SIZE`, `.bss`, nothing to do with
  the ESP heap) and **LVGL 9.5 cannot survive `lv_malloc()` returning NULL mid-build** -
  `lv_obj_class.c` stores each new child straight after an unchecked `lv_realloc()`, so an
  exhausted pool is a wild pointer, and the `LV_ASSERT_MALLOC` sites that do check it reach
  `LV_ASSERT_HANDLER`. Either way the display reboots, on a tap, in the owner's living room. The
  only defence is never to start a build that cannot fit — there is no "handle the allocation
  failure" answer available from outside LVGL, and it is worth being plain that `LV_ASSERT_HANDLER`
  is **not** one: `lv_assert_hook.cpp` names the failing address and the pool's state over serial
  and then restarts, because returning from it would hand LVGL the NULL it did not check. It is a
  last resort that the two guards below exist to keep unreachable, and a `[lvmem] LVGL assert at
  pc=…` line is the unambiguous sign that one of them was wrong.
  Measured on `cyd-3248S035R` 2026-09-16 (`GET /api/debug/ui`, bytes of pool per page):

  | stops | arrivals | night | stats | device | one page | all four at once |
  |---|---:|---:|---:|---:|---:|---:|
  | 2 (the owner's) | 19,280 | ~1,740 | 8,220 | 10,336 | 19,280 | 39,706 |
  | 4 | 31,656 | ~2,260 | 12,860 | 10,288 | 31,656 | 57,404 |

  The pool is 36,864 B, so the "all four" column is what the old switch (build the next page, then
  drop the previous one) asked for and could not have: on the owner's own two stops, the second
  tap — Stats → Device info — needed 10,336 B from a pool with 4,392 B left. One page at a time
  fits every configuration the panel guard allows, with 17.6 KB spare at two stops and 2.5 KB at
  four.
- Two guards sit under that rule. A page is **refused** if the pool no longer holds what that page
  cost the last time it was built (creep: a leak, or a page that grew), and a refused page returns
  to the arrivals page — never to the page the tap came from, so a tap can never strand the display
  somewhere secondary. And `createMainScreen()` **stops adding stop panels while the pool still has
  room**, showing `2 more stops will not fit in this display's memory` instead: the arrivals page is
  the first thing built at boot, before anything has measured it, so a stop list bigger than the
  board can draw would otherwise be a boot loop with the offending config still on disk. A stop
  panel costs ~6.2 KB on a 320-wide board and ~4.4 KB on a 240-tall one, which puts the ceiling at
  four stops and six respectively — under §6's maximum of eight.

  **Panels are not the same size**, which the first version of this guard assumed and the RC review
  caught. Each is sized from its own stop's `show` value, 1–4 rows (§6), six widgets a row, so a
  four-row panel costs roughly three times a one-row one — and estimating the next panel as "the
  largest built so far" let a config ordered small-panels-first admit a big panel on a small
  panel's measurement. `visibleStops()` order changes with the active profile (§6), so the same
  config could be safe in the morning and crash in the evening, and the crash is the boot loop
  above. The estimate is now `max(largest panel measured, rows × largest per-row cost measured)`,
  which is an upper bound for any row count given identical widgets per row. Measured in
  `ui-sim-pool` on 320×480 with four one-row stops followed by four four-row stops: the old
  estimate admitted six panels and finished on 3,704 B of pool, having let a 10,080 B panel in on a
  4,424 B measurement against a 512 B reserve; the scaled one admits five and finishes on 13,752 B.
  The `N more stops will not fit` caption is also built **before** the first panel and hidden,
  rather than out of whatever the loop leaves — it is the one allocation that must not fail,
  because it is the one that explains the failure.
- Pool exhaustion has its own simulator environment, because the normal one cannot show it:
  `pio run -e ui-sim` builds with a 512 KB pool for 64-bit host pointers. `ui-sim-pool` scales
  `LV_MEM_SIZE` to the board's by the measured host/board ratio (0.66, fitted against six figures
  from the owner's board and accurate to ~1.5 %), and `program <dir> pool` sweeps 2–8 stops across
  all four panel sizes, then again with non-uniform `show` values in both orders (the shape the
  paragraph above describes). It reproduces the four-stop arrivals page at 31,664 B against 31,656 B
  measured. `POST /api/debug/page` (§7) drives the cycle on real hardware.
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
  (`getPollStatus()`; the arrivals header only ever says "stale") - on the boards where the Data
  sources panel below fits, that line moves into it. A panel titled with the
  device name (`device.name`), `up 2d 5h` on its title row: the web PIN in the big minutes font
  with `web PIN` beside it (on a ≥ 320-tall board and up to 8 characters; a longer custom PIN
  takes the body font and wraps, never ellipsizes - a PIN the owner cannot read off the screen
  is a PIN they have lost), `SD  mounted, 3720 MB free` - or `N rows dropped: <error>` in red
  while writes are failing and amber once they land again (sd_logger.h, F26) - and
  `heap  75 KB free`. A *Data sources* panel when the height allows it - decided from the
  heights, like the stats layouts: one line per feed the config has on (`SEPTA`, `Weather`,
  `Indego`, `Alerts`; a feed that is off has no row), each a status word in the arrival colours -
  green `ok`, amber `stale`, red `failed`, grey `no data yet` - and `, 12 s ago` from the
  poller/weather/bike/alerts timestamps `/api/state` exposes (SEPTA is stale past two poll
  intervals + 30 s, weather past an hour (F29), Indego past 10 min like the bike strip, alerts
  past 15 min; a failed SEPTA poll carries the poller's reason). Two feeds per line on a
  480-wide board, so 480x320 fits it with a few px to spare; 320x480 has room for everything;
  on a board where it does not fit (every 240-tall layout, 240x320 with three or more feeds) the
  SEPTA line stays in the Network panel and the page is exactly as before. All panels are
  content-height (a mostly-empty card looks like a fault); `reset Wi-Fi: hold 5 s` is a bordered
  button along the bottom edge, the only clickable child on any page.
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

### 10.2 Reads that retry, and reads that give up

`GET /api/state` and `GET /api/config` are the two heap-gated reads (§12.1): during a poll cycle
they legitimately answer 503, and `/api/state` can come back as a 200 with a zero-length body.
`resilientRead()` folds 503, an empty 200, an unparseable body, a network error and a timeout into
one "busy" outcome and retries with capped, jittered backoff, sharing one in-flight request per
endpoint so several open tabs cannot amplify the pressure they are retrying against. A top-bar line
says so and clears itself.

It does **not** retry forever. A 4xx other than 429 is the device answering definitively, and
retrying one is how the blank page this mechanism exists to prevent comes back through a different
door: `renderNow` awaits `api.config()` before it fetches state, so a permanent 421 — browsing an
old `<name>.local` after the display was renamed — left the Now page on "Connecting to the
display…" for as long as the tab stayed open, with no error anywhere. Those reject, the view shows
the message, and the top-bar line switches from "retrying…" to what happened; 421 is worded for the
owner ("it has probably been renamed — open it at its current name or IP address") rather than
echoing the header. 429 stays retryable: it carries `retry_s` and means "later", not "no".

### 10.3 Honest numbers on the Stats page

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

### 10.4 Untrusted text

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

**Cross-site and rebinding.** The `Host` header is checked on every request before its handler
runs and must name the device (its IP, `<device name>`, `<device name>.local`, `192.168.4.1` or
`localhost`, optional port, case-insensitive); anything else is 421. "Before its handler" is the
exact claim, corrected in the RC review: the middleware chain runs only once a request's **body**
has been parsed (ESPAsyncWebServer 3.12.1 `WebRequest.cpp`, which switches to `PARSE_REQ_BODY` at
end-of-headers and calls `_runMiddlewareChain()` afterwards), while `handleBody`/`handleUpload` run
during the parse. So `POST /api/ota`'s upload callback used to reach `Update.begin()` and up to
1.7 MB of `Update.write()` before the chain answered 421 - the response was refused, the flash
writes were not. It now repeats the host check itself, at `index == 0`, before anything is written.
Not a live bypass even before that (the route is PIN-gated, and see CORS below), but "the check
gates the response" and "the check gates the side effect" are different properties. The JSON body
handlers are unaffected: `AsyncCallbackJsonWebHandler::handleBody` only `calloc`s a buffer and
copies into it, and the handler proper runs after the chain. That is what stops a page on
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

**Residual risks, accepted for now.** HTTPS to the data sources stays out of the shipping envs
(§2; §2.1 has the 2026-09-16 measurements and a heap-gated prototype behind `-DTRANSIT_HTTPS`: a
TLS session on this SDK is two fixed 16,717 B record buffers plus ~20 KB around them, and the
byte-addressable heap at the moment a fetch starts is 27-42 KB, so on the owner's board the gate
never once found room; a rebuilt SDK with smaller buffers is what would change that). So **the PIN
travels over the LAN in clear text** — 
anyone who can passively sniff the owner's own network, or who already controls a device on it,
can read it and then do anything the owner can. The PIN raises the bar from "any script that finds
the device" to "an attacker already inside the network with packet capture"; it is not a defence
against that second attacker. Firmware is unsigned, so anyone who *has* the PIN can flash
arbitrary code. SEPTA and the other feeds are fetched over plain HTTP, so their contents are
spoofable by that same on-path attacker (arrival times are not a secret, and nothing in the
response is executed).

### 12.1 Memory posture (2026-09-14)
The classic ESP32 has ~320 KB of DRAM and no PSRAM; with LVGL, Wi-Fi, the async web server and the
feature set of §6 there is **~40-46 KB of usable heap free at runtime** (`MALLOC_CAP_8BIT`,
restated 2026-09-16 - §2.1). The "~75-80 KB" this section carried until then was
`ESP.getFreeHeap()`, which also counts the 33,708 B IRAM heap that `malloc()` never hands out for
data; that figure is not wrong, it just is not memory anything here can use. Big long-lived objects are allocated
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
poll, largest block ~32 KB median - that heap figure is `ESP.getFreeHeap()`, i.e. **~20 KB of
byte-addressable heap** (§2.1), which is the number that explains why `/api/state` answers 503
during a plain HTTP fetch; re-measured over 114 clean samples on 2026-09-16 the usable minimum is 11.0 KB
with a median of 34.9 KB, and the largest block runs 2.9-47.1 KB, median 20.5 KB. **Invariant:** every long-running task that does STL allocation catches
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
16 in-flight `std::bad_alloc`s at 128 B each, well over the six the **three** allocating tasks
listed above can have mid-throw at once - one each, plus a dependent exception from a rethrow; the
file's own arithmetic, and the count that agrees with it) together with `__cxx_init_dummy`, so the SDK's `-u __cxx_init_dummy` is satisfied
by our object and `libcxx.a(cxx_init.cpp.obj)`, which carries the SDK's zero-returning definition, is
never linked. The file explains why `--wrap` cannot do this and why `--allow-multiple-definition` was
not used; the link map is the proof. Cost: one 2 KB `malloc` before `app_main()`, never freed; the
boot log prints it as `[heap] eh_pool arena=2048`. `POST /api/debug/oom` (PIN-gated, §7) is the
deterministic proof: it takes the heap away in shrinking blocks until even a 16-byte allocation
fails, forces a `std::bad_alloc`, frees everything and answers `caught:true`; without the pool that
request reboots the device. The device suite runs it, on a fresh boot, for the reason that follows.

The pool alone was not enough (found and fixed 2026-09-16). `__cxa_throw`'s first act is
`__cxa_get_globals()`, which keeps the per-task "exception in flight" bookkeeping
(`__cxa_eh_globals` - the caught/uncaught counters every catch block and the unwinder read) in
thread-local storage. On a task's *first* throw that storage does not exist yet, so libstdc++
(`eh_globals.cc`) does a plain `malloc` of it and calls `std::terminate()` outright if it fails -
past every `try`/`catch`, and before `__cxa_allocate_exception` is ever reached, so the emergency
pool is never even consulted. It is ~16 bytes, but a task's first throw is overwhelmingly a
`bad_alloc`, i.e. precisely the moment the heap is gone. Observed exactly so: on a fresh boot, with
`POST /api/debug/oom` - the pool's own proof - as the first request, the device aborted
(`abort <- __terminate <- __cxa_get_globals (eh_globals.cc:150) <- __cxa_throw <- operator new[] <-
handleDebugOom <- ... <- _async_service_task`), while the full suite passed, because by then an
earlier section had already made the AsyncTCP task throw and pay the allocation with the heap
healthy. The fix is to pay it deliberately and early: `warmExceptionGlobals()`
(`cxx_exception_pool.cpp`) calls `__cxa_get_globals()` and then throws and catches once, on the
calling task, while the heap is plentiful. It cannot be done once centrally - the storage is
per-task, so **every task that can throw must warm itself, on itself**, and any new one must too:
`main.cpp setup()` for loopTask (which is also the LVGL display loop), `net_poller.cpp
pollerTask()` (which covers the queued proxy and stats jobs, since they run on that task), and
`web_server.cpp`'s first-in-chain middleware for the AsyncTCP task, which the library creates and we
therefore cannot warm at its entry - it warms on request number one and costs one
`pthread_getspecific` per request thereafter. The middleware is not literally first on the request
path, though, and for one route that matters: the chain does not run until a request's body has
been parsed (§12, "Cross-site and rebinding"), so `handleOtaUpload()` warms on its own entry too.
Without that, a device whose *first* request is a firmware upload ran `checkPin()`, the heap gates,
`Update.begin()` and `otaFail()`'s `std::string` concatenations on a task that had never thrown -
precisely the cold-first-throw shape above. Warming only makes the throw catchable, so the upload
callback is also wrapped in its own `bad_alloc` guard: nothing in ESPAsyncWebServer catches what
escapes one, and `guarded()` does not reach it. Two details the implementation depends on and the
file documents: GCC folds a `try { throw 0; } catch (int) {}` whose handler it can see into a plain
jump, so the throw lives behind a `noinline` call, and both ABI entry points are declared
`__attribute__((const))` in `<cxxabi.h>`, so a call whose result goes unused is deleted - every
result is consumed. The built image is the proof (`objdump -d`: `__cxa_get_globals_fast`,
`__cxa_get_globals`, `__cxa_begin_catch`/`__cxa_end_catch` in `warmExceptionGlobals`,
`__cxa_allocate_exception` + `__cxa_throw` in `throwOnce`). The boot log prints
`[heap] eh_globals warmed on <task> (globals=…, throw path ok, free …)` once per task. Because
both halves are one-shot per task, the ORDER of the on-device proof is part of the test: section A
of the device suite reboots first and makes `/api/debug/oom` the first request that throws, since
run any later it answers `caught:true` whether the first-throw path works or not. The heavy read handlers (`/api/state`,
`/api/config`) still refuse up front with a fixed-literal 503 when byte-addressable free heap is under
`kMinHeavyResponseFree8` (12 KB, `MALLOC_CAP_8BIT` since 2026-09-16 - the 24 KB INTERNAL floor it
replaced could never fire, §2.1) or the largest block under `kMinHeavyResponseBlock` (7,924 B) - no longer because the failure
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
response object; it rethrows out of the handler - **this one stopped being hypothetical on
2026-09-17, see "the 503 for out of memory needs memory" below**), **an allocation the library
makes on its own frames after our handler has returned - the header list of a response, assembled
inside `_parseLine`; nothing we can write is on that stack, which is why the defence there is
admission control rather than a catch (below)** - and the pool being finite. Two instances of the
first are no longer hypothetical - both seen on 2026-09-16 while `/api/debug/oom` held the heap and
the poller was mid-fetch, and both `assert`-and-panic rather than failing soft: newlib's `_dtoa_r`
(`assert failed: dtoa.c:239 (REENT malloc succeeded)`, reached from the `snprintf("%f")` in
`refreshWeather`, on the poller task) and lwIP's `tcp_receive` (`tcp_in.c:1450`, on `tcpip_thread`).
Neither is a C++ exception and no pool or catch block can reach either; the only defence is not to
exhaust the heap while a fetch is in flight. That is a real constraint on the *test hook*, not on
normal operation - nothing else takes the whole heap on purpose - so the device suite fires
`/api/debug/oom` only in the quiet window just after a poll completes (section A0).

**"The 503 for out of memory needs memory" - the third instance, and it is ours (2026-09-17).**
The list above has carried "a catch block that itself allocates with nothing left (building the 503
response object)" as a known, accepted limit since the pool work. It is not hypothetical. The diag
board panicked at 78 minutes of uptime; the coredump was read off flash and decoded. Crashed task
`async_tcp`, "abort() was called":

```
abort <- std::terminate <- __cxa_throw <- operator new (sz=100)
  <- AsyncWebServerRequest::beginResponse        WebRequest.cpp:1212
  <- AsyncWebServerRequest::send(503, "application/json", "{"error":"out of memory, retry"}")
  <- the catch handler in transit_app::guarded()  src/app/web_server.cpp:1330
```

Read it bottom up and every step is this project working as designed until the last one: a handler
threw `std::bad_alloc`, `guarded()` caught it, and then the 503 it answered with **allocated** -
`beginResponse()` is `new AsyncBasicResponse(...)`, the sz=100 in the dump, plus an Arduino `String`
for the content type and one for the body. That allocation failed as well, out of a catch handler
with nothing outside it, and a throw that escapes a catch handler is `std::terminate`. It fires
precisely when the heap troughs during a web request; `min_free8` since boot on that board was
1,452 B. The exception pool and the per-task warming cannot help: they make a throw *catchable*, and
this throw is in the code that was doing the catching.

Two layers, because neither is sufficient alone, and both are in `src/app/heap_reserve.h` /
`oom_reply.h`:

1. **A 1 KB reserve, held from boot.** Taken before Wi-Fi alongside the other long-lived
   allocations, and simply held. Every reply that exists *because* memory ran out - `guarded()`'s
   503, `refuseIfLowHeap()`'s 503, `sendJson()`/`sendJsonStreamed()`'s truncated-document 503, the
   401/429s, the Host check's 421, the queued jobs' 503s - frees it immediately **before** calling
   `request->send()`, so the response object has somewhere to come from. It is re-armed from a
   context that is *not* the handler that spent it (the poller's idle slice and poll-start) and only
   when `MALLOC_CAP_8BIT` free is at least 20 KB with a 4,340 B largest block - deliberately well
   above the 12 KB / 7,924 B gate it exists to get past, so re-arming can never be the allocation
   that pushes a device back under the floor it just recovered over. The arm/disarm decision is a
   pure function, host-tested (`pio test -e native -f test_heap_reserve`), the same shape as
   `poller_liveness.h` and `proxy_queue.h`. Cost: 1 KB that is deliberately unavailable at rest.
2. **A nested `try`/`catch` around the send.** A throw inside a catch handler *is* catchable by a
   `try` block nested inside that handler - which is what makes this legal where an outer guard is
   not, and there is no outer guard on the AsyncTCP path, which is the whole problem. If the send
   still cannot be built, the connection is closed with no reply and `oom_replies_dropped` counts
   it on `GET /api/debug/ui`. A dropped connection is a deliberate answer: the web app's
   `resilientRead()` (SS10.2) already treats one as a retryable failure. `heap_reserve_held` is
   reported beside it.

The rule for anyone adding an error path: **a reply that exists because memory ran out goes through
`sendUnderPressure()`**, its body is a fixed literal, and anything that has to be formatted first is
formatted into a stack buffer (which is why `sendFailure()` already did).

**"The reply is assembled later, on the library's frame" - the fourth instance, and it is the one
neither defence above can reach (0.3.1-rc3).** The entry immediately above adds a 1 KB reserve and a
nested `try`/`catch` so that a reply which exists *because* memory ran out can still be built. That
fixes the path it was written for and **does not fix this one**, and the difference is worth being
precise about. The rc2 device suite crashed under section E (three rounds of seven concurrent
requests: 4 x `/api/state`, `/api/proxy/stops`, `/api/stats`, one asset). Decoded backtrace, task
`async_tcp`:

```
abort <- std::terminate <- __cxa_throw <- operator new
  <- std::list<AsyncWebHeader>::emplace_back
  <- AsyncWebServerResponse::addHeader <- AsyncAbstractResponse::_assembleHead
  <- AsyncWebServerRequest::_respond <- _send <- _parseLine <- _onData <- AsyncClient::_recv
```

The response object handed to `request->send()` is only **stored**. The library assembles its header
list **later**, when the request completes, from inside `_parseLine` - a frame with no handler of
ours anywhere above it. By then `sendUnderPressure()` has returned, the reserve it released has been
consumed, and its nested `try`/`catch` is nowhere on that stack. **Both defences are structurally
out of reach of this allocation, and no defence written in our code can reach an allocation the
library makes on its own frames.** Keep them anyway - they close the path they were written for -
but do not read them as covering this one.

What is left is to not have that many requests in flight at once, which is a decision that can be
taken at **accept**, where a refusal costs nothing: no request object, no response, no header list,
no send buffer. `src/app/admission.h` holds the rule as pure arithmetic (host-tested);
`web_server.cpp`'s `GatedWebServer` holds the machinery.

- **Where the hook is.** `AsyncWebServer::_server` is `protected`, so a subclass can re-register the
  `AsyncServer` `onClient` callback that the base constructor installed. The replacement is the
  library's own body plus the check in front and a `try`/`catch` around it - the library's version
  has neither, and a throw on that task is `std::terminate` like everything else in this list.
- **The floors are derived, not chosen.** One in-flight request costs about 5,028 B
  (`AsyncWebServerRequest` 356 + `AsyncClient` 196 + `tcp_pcb` 208 + the parsed Strings + the
  response object) and its chunked send buffer another 2,872 B, so the free-heap floor is their sum,
  **7,900**, and the largest-block floor is 1.5x the send buffer, **4,308**. Neither is a multiple of
  512, for the lattice reason SS2.1 gives.
- **The cap is on COUNT, because a heap reading at accept cannot see the future.** At accept a
  request has cost only ~760 B; its document, response and send buffer come later. A free-heap floor
  therefore cannot refuse the seventh request of a burst on the strength of what the first six are
  about to do. Only a count can. It is **5**, and that is an acceptance constraint rather than a
  preference: the suite allows at most six refusals across the three rounds ("E at most 2 refusals
  per round"), and a cap of N refuses about (7 - N) per round, so anything below 5 fails that check
  by construction however well it protects the heap. A host test asserts the relationship. Note that
  the check's own comment says "Not connection-count exhaustion: nothing here caps concurrent
  clients" - true when it was written, no longer true; if a run still shows more than six refusals
  the budget is what needs revisiting, not the floors.
- **In-flight requests are counted with `weak_ptr`s, not a counter**, because there is nowhere to
  put a decrement: `AsyncWebServerRequest`'s constructor overwrites every `AsyncClient` callback,
  and `request->onDisconnect()` is a single slot three call sites here already own (the proxy file
  lease, the OTA guard, the log-reader lease). `create()` returns a `shared_ptr` whose control block
  *is* the request's lifetime, so a `weak_ptr` expires exactly when the request is gone - no hook,
  no collision, and nothing allocated to copy or test. The array is swept at accept and in
  `HostGuardHandler::canHandle()`, both on the AsyncTCP task, so it needs no lock;
  `in_flight_requests`, `admission_refusals` and `max_in_flight_requests` report it on
  `GET /api/debug/ui`.
- **The floors apply only under contention: the only connection is always admitted (0.3.2-rc1).**
  rc3's rule applied both heap floors unconditionally, and on the owner's board at v0.3.1 that
  turned a fragmented heap into a total lockout. Measured 2026-09-17, uptime 2,646 s: the largest
  free block had fallen to **3,444 B**, under `kAcceptMinLargestBlock` (4,308), so the accept path
  refused **every** new connection - `GET /api/debug/ui`, which is deliberately outside
  `refuseIfLowHeap()` so that it still answers when the heap is gone, and `POST /api/reboot`, which
  is the recovery path, along with everything else. The board answered ping and nothing else until
  the heap-wedge self-heal rebooted it five minutes later. That is a worse outcome than the crash
  the floors prevent, and it is not a trade that had to be made: the rc2 abort needed **seven**
  requests alive at once, and a single request cannot reproduce it - nothing else is competing for
  the heap, and if its own reply will not fit, `guarded()` catches the throw and answers 503 out of
  the 1 KB reserve. The floors are therefore a statement about *contention*, and with no contention
  they have nothing to say. The rule is now: `in_flight >= cap` refuses (unchanged);
  `in_flight == 0` admits whatever the heap says; otherwise both floors apply exactly as before.
  The count cap is unchanged at 5. `test_admission` pins the four-row table so a future edit that
  reinstates the lockout fails on the host rather than on the hardware.

**The nightly restart, and the honest sentence about it (owner decision, 0.3.2-rc1).**
`device.nightly_restart` restarts the board at a chosen local time, **on by default at 03:30**.
`src/app/nightly_restart.h` holds the rule, pure and host-tested (`test_nightly`, 8 cases);
`main.cpp` reads the clock once a second from the display task, outside `loop()`'s `bad_alloc`
guard and beside the liveness check, and goes out through the same `scheduleRestart()` the reboot
endpoint uses.

**It is a mitigation, not a fix, and this document should not be read as saying otherwise.** This
board has no PSRAM, a ~100–160 KB heap, and nothing that defragments a running one. Two releases of
work have gone into the per-cycle contiguous demand and 0.3.2-rc1 removes every request above
~1.2 KB from a cache-hit cycle — and none of that is a guarantee, because fragmentation is
cumulative and the trigger is still unknown: the owner's board rested at a 22.5 KB largest block for
thirty-five minutes and was at 3,444 B forty minutes later, and nothing in the record says why. A
boot is the only defragmentation this hardware has. Taking one at 03:30, when nobody is reading a
transit display, costs a few seconds of uptime and starts every day on a heap in one piece. It does
not excuse leaving the real cause unfound, which is what the per-cycle log above exists for.

The guards are each there for a specific failure: **one minute, once** (the check acts only on the
transition into the matching minute, so a slow minute cannot fire it twice); **uptime > 1 h**
(without it a board that boots at 03:29 restarts at 03:30, comes up, and can loop); **a sane clock**
(before NTP the local time is 1970 and "03:30" matches a moment with nothing to do with 03:30); and
**not during an OTA** (the same hazard the wedge and liveness nets stand down for). An unparseable
time parses to −1 and the rule treats −1 as *never* — for a rule that restarts the device, the
failure direction is always "do not restart". The restart is recorded in the RTC note, so
`GET /api/state`'s `last_restart.reason` reads `nightly` rather than leaving a bare `ESP_RST_SW`
for someone to puzzle over.

**The heap-wedge self-heal did not fire, and the reason was the failure backoff (0.3.2-rc1).** On
2026-09-17 the owner's board sat with every poll failing at `oom-transit`, largest free block
3,444–4,596 B, `wedged_polls` reading 4 at 15:50 — and it was still in that state at 16:07, when it
had to be hard-reset over USB. The expectation was "15 wedged polls at 30 s → a restart by 15:56".
The trace, because the answer is not the obvious one:

1. **The tally was not being reset.** `wedged_polls` = 4 was correct; only about four cycles had
   failed by then. (`failed_polls` = 11 counts every failure since boot, not consecutive ones.)
2. `getPollStatus()` returning a default-constructed `PollStatus` on a lock miss does not break the
   rule either: `PollStatus::ok` defaults to **false**, which is the direction that *advances* the
   tally. Ruled out.
3. The second publish of a cycle copies `last_poll_ok` from the published Snapshot, so it cannot
   overwrite a failure with a success. Ruled out.
4. **The failure backoff.** A failed poll drives `nextIntervalS()`'s backoff, which saturates at
   `kBaseBackoffS << 3` = **240 s**. Fifteen consecutive failed polls is therefore
   30 + 60 + 120 + 12 × 240 = **51 minutes**, not seven and a half. The board was doing exactly
   what was written; the threshold had been written against a cadence a failing board does not run
   at. The old comment's "which with the failure backoff is several minutes" was out by a factor
   of eight.
5. The poller-stall net in `main.cpp` could not help and was not meant to — the poller *was*
   completing cycles, and that net's window is a multiple of the interval the poller is actually
   running at, backoff included, so it stretches with the backoff too.

`src/app/wedge_policy.h` now holds the rule, pure and host-tested (`test_wedge`, 8 cases), with two
tallies:

- **Out-of-memory cycles are counted directly, at the catch site.** A cycle that caught
  `std::bad_alloc` knows that about itself; the old rule threw that fact away and re-derived it
  from a largest-block reading taken afterwards, which is a second condition that can lapse on its
  own. **Three consecutive** such cycles restart the board — about 3½ minutes with the backoff,
  against the 51 the old rule really cost. A lossless restart beats a stale display: the arrivals
  are refetched within seconds of boot, the stats log is on the SD card and the config on LittleFS,
  and the RTC note records why (`last_restart.reason` = `heap_oom`).
- **The old failed-poll-plus-small-block rule is kept at fifteen** as the slower backstop for a
  wedge that never throws. Its condition can also be met by an ordinary SEPTA outage on a board
  whose heap merely happens to be busy, so it must stay hard to trip.

Both tallies stand down and are *forgotten* during an OTA, for the reason the liveness net does the
same. `oom_streak`, `wedged_polls` and `wedge_reason` on `GET /api/debug/ui` say how close each is.
The wedge check reads a lock-free mirror of the cycle's own verdict rather than calling
`getPollStatus()`, which takes the poller's lock and copies a `std::string` on a line that has no
`try` above it.

**The error-reply reserve could not come back either.** Through the whole wedge `heap_reserve_held`
read false: free8 17–20 KB with a 3,444 B largest block is under *both* of the old re-arm
thresholds (20,480 and 4,340), so the kilobyte that exists to make an out-of-memory 503 possible was
unavailable in exactly the state it is for. The floors are now **13,556** free8 — the smallest value
on the 512-byte lattice that still clears `kMinHeavyResponseFree8` by more than the reserve is big,
which is why it is not simply 12 KB — and **2,292** for the block, twice the 1,024 B the reserve
actually has to be carved out of rather than the 4.2× it was.

**Two hours of per-cycle history, because the trigger is still unknown (0.3.2-rc1).** The per-stage
ring (`heap_trace.h`) holds 64 entries, which on a healthy path is **three cycles**. It is what
identified the failing allocation on 2026-09-17 — the ring read `pre-transit → oom-transit` with no
stage between — and it is structurally unable to say what happened *before*: the board rested at
free8 39–40.7 KB with a largest block never under 22.5 KB for thirty-five minutes and was then
found at free8 17–20 KB with a largest block of 3,444 B, and every cycle in which that happened had
scrolled out of the ring. `src/app/cycle_log.h` adds **one 16-byte row per cycle, 240 of them** —
two hours at a 30 s cadence, far more once the failure backoff stretches the interval — carrying
the poll-start `free8` and largest block, the lowest `free8` sampled anywhere in that cycle, and a
flag word for the four things that do *not* happen every cycle (schedule refetch, alerts fetch,
weather, the 400 KB Indego stream) plus out-of-memory, poll-failed and clock-unsynced. If the
collapse correlates with one of those, this is the record that shows it. 3,840 B of `.bss`, nothing
on the heap, and it is served by `GET /api/debug/ui?log=1` through the same fixed-buffer +
`serialized()` route the stage ring uses, so it cannot be the thing that stops that endpoint
answering.

The "lowest free8" column is honestly a **floor of the samples this firmware takes** — every stage
boundary, plus every accept-time admission check on the AsyncTCP task — not a true minimum; a
trough that opens and closes between two samples is invisible to it, which is exactly how rc1's
696 B reading was missed by the stage ring. `heap_caps_get_minimum_free_size()` is the true
minimum and is reported separately as `min_free8`, but it is since *boot* and cannot be attributed
to a cycle. Read the two together: this column says which cycle was tight, that one says how tight
the board has ever been.

**And the background jobs had to stop starting inside a burst.** The same run showed why a heap gate
alone is the wrong question: `[proxy] refusing a queued job: not enough heap to start it (free8
19528, largest 11252)` and the board aborted regardless. `idleWorkHasHeadroom()` describes *this
instant*, and at the start of a seven-request burst it is true while every one of those requests
still has its document and send buffer ahead of it. The queued stats/proxy gate now also requires
`inFlightRequests() <= 1` (one being the job's own paused request), and a job that cannot start is
**deferred** - put back at the front of the queue and reconsidered next idle slice - rather than
refused, because a burst passes in seconds. The deferral is bounded (`kMaxJobDeferrals`, ~6 s,
deliberately under the ~8 s a client waits) and then becomes a 503: a queued job holds a paused
request whose server-side timeout the library switched off, so "wait" must always become an answer.
That is the hang above, and it does not come back.

**A fifth case was in ESPAsyncWebServer's own middleware plumbing, and it is fixed by not having
any middleware (2026-09-16).** It belongs in this list because it is where a reader will look for
it, and because the shape is the one this section keeps meeting: a throw that the pool and the
warming make *catchable* while nothing anywhere catches it. The release candidate's device suite
failed its "no reboot during the page cycling" check on

```
__terminate <- std::terminate <- __cxa_throw <- operator new <- std::function copy
  <- AsyncMiddlewareFunction::run <- AsyncMiddlewareChain::_runChain   Middleware.cpp:70
  <- _runMiddlewareChain (WebRequest.cpp:1038) <- _parseLine <- _onData
  <- AsyncClient::_recv <- _async_service_task
```

Read against the pinned source, `_runChain` costs **three small heap allocations per request per
middleware**, and it is worth writing them down because the arithmetic is what decides which fixes
are available. It builds a `next` `std::function` capturing five things *including a copy of the
finalizer* - 32 bytes, past libstdc++'s 8-byte small-object buffer on this ABI (`_Nocopy_types` is
a union whose largest member is an 8-byte pointer-to-member-function), so heap. It calls
`m->run(request, next)`, whose parameter is `ArMiddlewareNext` **by value**: a second copy, a second
allocation. `AsyncMiddlewareFunction::run` then calls `_fn(request, next)`, by value again: a third,
and the one the backtrace names. None of them is catchable: `_runChain` has no `try`, nor does
anything above it up to the AsyncTCP service task, and there is no user hook on that path at all.

The cost being *per middleware* rules the obvious mitigation out rather than in: merging the Host
check and the exception-globals warming into one registration was already done in the RC review, so
there was one middleware and three allocations, and there was no second one to merge away. Nor can
our middleware "allocate nothing" its way out - our code allocates nothing already; the throwing
allocation is the library copying the chain's own `std::function` before our code runs. And the
library is not vendored or patched, by policy.

What is available is the first line of `_runChain`: `if (!_middlewares.size()) return finalizer();`.
With nothing registered on the server and nothing on any handler, the chain is that branch: no
`std::function` is copied, nothing is allocated, and the terminate site is **unreachable rather than
unlikely**. So the Host check and the warming moved to `web_server.cpp`'s `HostGuardHandler`, an
`AsyncWebHandler` registered first, whose `canHandle()` warms the task and claims the request only
when the `Host` is not one of ours (and whose `handleRequest()` then answers 421 from a fixed
literal). `AsyncWebServer::_attachHandler` calls `canHandle()` on each handler in registration order
at **end of headers** - see `_parseLine`, which does `_attachHandler` before switching to
`PARSE_REQ_BODY` - so this is not merely an equivalent hook, it is an *earlier* one.

Three things fall out of that, and the first is the answer to this section's own standing note that
the chain runs after body and upload callbacks:

- **`handleOtaUpload` stops repeating the Host check and the warming.** It carried both only because
  the chain runs late; the guard runs before the first body byte reaches the callback, so a
  DNS-rebound upload is claimed by the guard and never becomes an OTA request at all - it cannot
  reach `Update.begin()` or one `Update.write()`. The callback keeps its own `bad_alloc` guard,
  which is a different job and still nobody else's.
- **Every request does two or three fewer heap allocations**, because calling `hostAllowed()` from
  `canHandle()` means it must not throw, so the string work moved to `src/app/host_match.h` and
  works in fixed buffers: no `std::string` of the header, no `g_host_name + ".local"`, no `String`
  from `WiFi.localIP().toString()`. Small, but it is per request, on the device whose resting
  largest block is the open problem below.
- **A bad-Host request with a body now has that body read and discarded before the 421**
  (`isRequestHandlerTrivial()` stays `true`, so the bytes are counted, never parsed, and no callback
  sees them), where the OTA-specific check used to hang up on the first chunk. Stated rather than
  hidden. No side effect happens either way; making the handler non-trivial to get the hang-up back
  would turn a hostile `text/plain` body into parsed form parameters, which is the worse trade.

The proof is in the image, not in the argument: `AsyncMiddlewareFunction::run`, its destructors and
`AsyncMiddlewareChain::addMiddleware` are all present in the RC's ELF and **all absent** from this
one - nothing constructs a middleware, so `--gc-sections` drops them, and the frame the backtrace
names does not exist in the build. `_runChain` itself is still linked and can only take its empty
branch. The three finalizer `std::function`s that remain capture one pointer each; `objdump` shows
their `_M_manager` bodies contain no calls at all, which is libstdc++'s stored-locally path.

**Do not add a middleware back.** `g_server.addMiddleware(...)` is a one-line change that
reintroduces an uncatchable reboot, and it will look completely reasonable to whoever writes it.

A **fourth instance, and the one that matters for normal operation**, turned up on 2026-09-16 on a
clean `dcb6353` image with `/api/debug/oom` never fired once (the capture was grepped: the only
`oom` matches are the bootloader's `ho 8 tail 4 room 4` line matching inside the word "room"). Free
heap decayed to ~34.5 KB with the largest block down to 164 B under light polling, `main.cpp
loop()` caught its `bad_alloc` and skipped frames exactly as designed, and lwIP then panicked
underneath it: `assert failed: sys_timeout_abs ... timeouts.c:194 (sys_timeout: timeout != NULL,
pool MEMP_SYS_TIMEOUT is empty)`. The C++ machinery worked; the C layer below it did not have to.

**Do not "fix" that by raising `MEMP_NUM_SYS_TIMEOUT` - there is no pool to raise.** The ESP32
lwIP port sets `MEMP_MEM_MALLOC 1` and `MEM_LIBC_MALLOC 1` unconditionally
(`framework-arduinoespressif32-libs/esp32/include/lwip/port/include/lwipopts.h:98,105`), so every
`memp_malloc()` is a `mem_malloc()` and therefore a plain libc `malloc()`. Stronger still: under
`MEMP_MEM_MALLOC`, `LWIP_MEMPOOL_DECLARE(name, num, size, desc)` (`lwip/memp.h:69`) discards its
`num` argument and emits a `memp_desc` carrying only the element size - **no static pool array is
emitted at all**, so for `MEMP_SYS_TIMEOUT` there is no pool object in the image that could be
empty. "Pool ... is empty" is a fixed string lwIP prints on a failed allocation that was a plain
`malloc`. And there is no knob to turn even if you wanted one: `MEMP_NUM_SYS_TIMEOUT` is not
defined anywhere in the esp32 port headers, so it already sits at lwIP's own default in `opt.h`.
Anyone who goes looking will find nothing, conclude the header is missing it, and add one - the
worst version of this dead end. So this is
not a sizing bug and not an lwIP bug - it is the heap fragmentation arriving at whichever caller
asks next, and lwIP asserts instead of failing soft. Every `MEMP_NUM_*` figure in this SDK reads as
a limit and behaves as a formality; judge memory questions from `MALLOC_CAP_8BIT` free and largest
block, never from those.
 Firing it as
soon as a freshly booted device answered, with the first schedule/weather/bike fetches still in
flight, panicked the board 2 times out of 2; firing it right after a poll finished was clean 3
times out of 3. The convergence that
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
(stats summaries, queued proxy jobs) additionally waits for 16 KB of `MALLOC_CAP_8BIT` free and a
12,020 B largest block so it never collides with a config save on the web task. (The "40 KB free
heap" that line carried was `ESP.getFreeHeap()`, i.e. `MALLOC_CAP_INTERNAL`, which includes ~34 KB
of 32-bit-word-only IRAM heap — so it asked for about 6 KB of the heap an allocation can actually
use and almost never fired; the largest-block half had been doing the work alone. Both halves read
`MALLOC_CAP_8BIT` since 2026-09-16, §2.1.)

**The stats/proxy job queue: always dequeue, always answer (0.3.1).** That gate is the right
question for "may I start a ~9 KB `StatsAggregator` or stream 400 KB through LittleFS?" and the
*wrong* question for "may I look at the queue at all", and for a while it was asked as one:

```
if (idleWorkHasHeadroom() && runQueuedProxyJob()) { ... }
```

`&&` short-circuits, so on a heap that could no longer clear the gate the queue was **never read**.
A queued job is not inert. `proxy_worker.cpp`'s `enqueue()` calls `AsyncWebServerRequest::pause()`,
which in the pinned library returns a `shared_ptr` that keeps the request alive **and** calls
`client()->setRxTimeout(0)` — it turns the server-side deadline off. So the job sat there for the
rest of the device's uptime pinning a request object, an `AsyncClient` and an lwIP pcb; the client
waited until *it* gave up (observed as an 8 s hang with no reply); and once both of the two slots
were held, every later `/api/proxy/*`, `/api/stats` and `/api/stats/overview` got a prompt but
permanent `503 "proxy worker busy, try again"` — permanent because the only thing that could free a
slot was the caller that was gated off. Measured at 13.7 KB free8 with a 3,444 B largest block, both
halves of the gate failing. The asymmetry is what made it the worst available failure shape:
`/api/state` and `/api/config` answer a cheap 503 below a *lower* floor, while the endpoint that
needs *more* memory had no entry gate at all and degraded into silence.

Three changes, and the rule is now written down where it can be read and tested
(`src/app/proxy_queue.h`, pure and host-tested, the same shape as `poller_liveness.h`):

- **Dequeuing is unconditional.** `runQueuedProxyJob()` takes the gate as an argument and disposes
  of the job on every path: drop it if the client has gone, answer `503 {"error":"low memory,
  retry"}` if there is no headroom to start it — which releases the paused request and returns the
  slot — otherwise run it. There is no "leave it on the queue" outcome, and the host test asserts
  that there is not.
- **`/api/stats` and `/api/stats/overview` take the same `refuseIfLowHeap()` entry gate** the heavy
  reads do, at the same thresholds, so a low-heap device answers promptly instead of queueing work
  it cannot start.
- **`enqueue()` refuses a request whose client has already disconnected**, rather than pausing one
  nobody is waiting on.

The 2-slot queue semantics are otherwise unchanged. `proxy_queue_depth` in `GET /api/debug/ui`
used to be able to pin at 2 forever; it now means "the poller is not running its idle slices".

**And every route is inside `guarded()` (0.3.1).** This § asserts that the AsyncTCP handlers catch
`bad_alloc`. Only three of about seventeen actually did: `/api/reboot`, `/api/wifi/reset`,
`/api/proxy/*`, `/api/rail/stations`, `/api/stats`, `/api/stats/overview`, `/api/log/index`,
`/api/debug/page`, `/api/debug/tap`, `/api/debug/oom` and `onNotFound` (which is where
`handleLogDownload` runs) all built `JsonDocument`s or `std::string`s with nothing catching above
them — an uncaught-throw reboot each, at a 3,444 B largest block. `/api/rail/stations` also moved to
`sendJsonStreamed()`: 149 station names is a 2-4 KB body, and `sendJson()` needed that many bytes
contiguous *twice* (a `String`, then `AsyncBasicResponse`'s copy of it).

**The watchdog fired again (2026-09-16), and the rule has a chokepoint and a number now.** The
paragraph above states the rule - the poller must not starve IDLE0 - and fixes the one call site
that had broken it. This is the second breakage, and like the `vTaskPriorityDisinherit` pair below
it was a class with one instance fixed rather than a fixed bug. The release candidate's suite
decoded:

```
decomposeFloat (ArduinoJson FloatParts.hpp:57) <- TextFormatter::writeFloat
  <- JsonSerializer<...>::visit... <- serializeJson
  <- runStatsJob   proxy_worker.cpp:293
  <- runJob (:346) <- runQueuedProxyJob (:388) <- pollerTask (net_poller.cpp:1213)
```

**The floats are not the cost, and reading the top frame as the cause would have produced a fix that
changed nothing.** Measured on the owner's board: `GET /api/stats?days=30` is a **5,027 B** document
with **28** floats in it and `GET /api/stats/overview?days=30` is 5,096 B with 87. Formatting those
is milliseconds. What the same measurement shows is that the request takes **5.3-7.8 s end to end**,
essentially all of it the month-of-CSV scan. The watchdog does not time a function, it times how
long IDLE0 has not run: the whole job ran on the poller task without ever blocking, so the panic
fired at the 5 s mark wherever the CPU happened to be, and `serializeJson` - the most CPU-dense
stretch, right at the end - is where it happened to be. Reducing float precision, or yielding only
during serialisation, would have left the number it fired on untouched.

**The primitive is `vTaskDelay(1)`, and the two obvious alternatives are no-ops here.**
`taskYIELD()` asks the scheduler for the highest-priority READY task, which is the poller itself;
`vTaskDelay(0)` is documented as exactly equivalent to `taskYIELD()`. Neither lets a *lower*-priority
task run, and IDLE0 is priority 0 against the poller's 1 - so neither does anything for this
problem at all. Only a delay that actually Blocks the caller gives IDLE0 the core. This is the same
fact the HTTPClient paragraph above records from the other direction ("yields only to
same-or-higher-priority tasks"); it is easy to write the wrong one of these three and impossible to
notice, because the wrong ones compile, run, and look like they are yielding.

**The chokepoint is `sd_logger.cpp streamLogLines()`**, and that is what makes this a mechanism
rather than another convention. Every CSV scan in the firmware goes through it - the stats page's
per-stop summaries, `/api/stats`, `/api/stats/overview`, the log export - and it owns the loop. A
job author never writes that loop, so a job author cannot forget the yield. `src/app/cpu_yield.h`
holds the budget (40 ms, i.e. 1/125th of the watchdog, `static_assert`ed against it and covered by
`pio test -e native -f test_cpu_yield` including the 49-day `millis()` wrap) and the reasoning
above; the cost is one 1 ms tick per 40 ms of work at `CONFIG_FREERTOS_HZ=1000`, about 2.4 %,
against a scan that already takes seconds.

**And it is measured, for the same reason `lock_misses` and `tick_ms_max` are.** `CpuYielder` keeps
the longest gap between two yields since boot and `GET /api/debug/ui` reports it as
`cpu_stretch_ms_max`, against the watchdog's 5,000. Measured on the owner's board: a single
`GET /api/stats?days=30` on a fresh boot takes **5.2 s** of wall time and leaves the high-water at
**52 ms** - one budget plus the tail of the line that was in flight when it expired. That pair is
the whole finding in two numbers: the job is exactly as long as it always was, and the longest the
poller now holds core 0 is a hundredth of it, against a watchdog that panics at 5,000 ms and a job
that used to run all 5,200 of them uninterrupted. A full suite run, which adds the overview scan,
the per-stop summaries and a month's log download, reaches 53-55 ms. A future loop that
does not yield shows up as a number climbing rather than as a panic on somebody's serial console,
and the device suite can fail on it. A loop that is not routed through the yielder is still invisible to this - the measurement
covers the rule's chokepoint, not the whole task - which is why the chokepoint matters more than
the number.

Separately, the stats jobs stopped serialising on the poller at all: they hand the document to
`sendJsonStreamed()` (`src/app/json_response.h`, moved out of `web_server.cpp` so the queued jobs
can reach it), whose chunked filler runs on the AsyncTCP task one bounded send-chunk at a time as
the socket drains. That also drops the `String` and `AsyncBasicResponse`'s copy of it - two
body-sized allocations per stats request. It is safe only because these documents are ~5 KB, i.e.
two or three 2,872 B chunks: the filler re-walks the document per chunk, so a document of tens of KB
would concentrate nearly a whole serialisation into the last chunk's call and move the starvation to
the other task rather than removing it. `json_response.h` says so at the top.

**`vTaskPriorityDisinheritAfterTimeout` came back (2026-09-16), and the rule now has enforcement
rather than convention.** The paragraph above records the first occurrence and treats it as cured by
the poller's priority plus "the capped read". It was not: what that pass capped was `getSnapshot()`,
and it capped it at the one call site that had just crashed. Running the release candidate's device
suite panicked a board again, from a different accessor on the same task:

```
panic_abort <- esp_system_abort <- __assert_func
  <- vTaskPriorityDisinheritAfterTimeout (tasks.c:5261)
  <- xQueueSemaphoreTake (queue.c:1842)
  <- transit_app::getBikes()                  bike_service.cpp:83   (a 500 ms wait)
  <- transit_app::ui::refreshMainScreen(...)  main_screen.cpp:764
  <- transit_app::ui::tick()                  ui.cpp:628
  <- loop()                                   main.cpp:308
```

Six accessors reachable from `ui::tick()` were still on 500-1000 ms waits - `getBikes()` twice,
`getWeather()`, `headerWeatherText()`/`Temp()`/`Icon()`, `stopWeatherNote()`, `getSdStatus()`,
`getActiveConfig()` - so this was a class with one instance fixed, not a fixed bug. §5 has the
mechanism that replaces the convention (`src/app/ui_lock.h`, one policy, decided by which task is
asking) and the last-good fallback that makes a refused read cost one frame.

The part worth writing down for the next person, because it is counter-intuitive: **a shorter
timeout is not a smaller version of this bug.** `xQueueSemaphoreTake()` reaches
`vTaskPriorityDisinheritAfterTimeout()` only on a take that actually blocked, then timed out, having
donated priority on the way in - so capping a wait at 50 ms makes a contended take time out *more*
often than 1000 ms would, not less. The 50 ms caps were a latency fix that read like a safety fix.
Only a **zero** wait is immune, and it is immune by construction: FreeRTOS's zero-tick branch
returns before blocking and asserts `configASSERT( xInheritanceOccurred == pdFALSE )` on the way
past, so the disinherit path cannot be reached from it at all. That is why the display task's budget
is zero rather than small.

The other half of the same finding is that the contention was mostly self-inflicted. `getBikes()`
was blamed on "the poller holds this mutex across a 400 KB Indego feed" - it does not, and never
did: `refreshBikes()` scans the feed with nothing held and takes the lock only to publish. What WAS
held across an allocation was `publishSnapshot()`, which copied the whole Snapshot under the poller's
mutex, and `getSnapshot()`, which copied it back out under the same one, once a second on the
display task and once per `/api/state` on the web task. The Snapshot is now published as a
`shared_ptr<const Snapshot>` prepared outside the lock and swapped in; readers take a refcount, not a
copy. `GET /api/debug/ui` reports `lock_misses` and `tick_ms_max` so the result is measured rather
than assumed - on the owner's board a tick is 5 ms.

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

**That last sentence is wrong, and was measured wrong on 2026-09-16.** The owner's board, on the
release-candidate image, with no suite running and nothing but a handful of hand-issued `curl`s
against it, reached 812 s of uptime with `ESP.getFreeHeap()` at 50,924 and the **largest block at
5,108 B** - under `kMinHeavyResponseBlock` (7,924 B), so `GET /api/config` and `GET /api/state`
answered `{"error":"low memory, retry"}` twelve times in a row over fifty seconds. A reboot cleared
it instantly and it began decaying again. So the wedge is not a property of the stress section; the
stress section only gets there sooner. Free heap is fine and stays fine - it is the largest block
that decays with uptime, which is why a reading of `ESP.getFreeHeap()` makes this look healthy
right up until a read is refused.

**The self-heal cannot see this state**, and not for the reason the paragraph above gives: polls
keep *succeeding* (their buffers are small enough to fit the gaps), so the consecutive-failure tally
never climbs and the reboot never fires. What is refusing is the heavy read handlers, which are not
what the tally counts.

**It is a band, not a latch - but the recovery time is not bounded by anything we know.** This is the
part that is easy to get wrong in both directions.
The largest block oscillates around the 7,924 B gate under load, and an eager client holds it below:
both observations above came from clients retrying every 1.5-4 s, which is itself the allocation
pressure they were waiting out. It recovers WITHOUT a reboot once that eases - measured in the same
run, 2,932 B during the config round-trips to 8,692 B a section later, no restart in between. So
"unreadable until you reboot it" would be an overstatement, and the earlier reboot that appeared to
cure it was never tested against simply waiting.

Equally, do not read "it recovers" as "it recovers promptly". In the 2026-09-16 paced run the suite
deliberately idled for **150 s** before the config round-trips and the largest block was still
5,108 B - the same figure as the session's first observation, and under the gate - so that settle
check failed. It had come back by the next section. Nothing here defragments on demand: the
recovery is real, its timing is not predictable, and any future fix has to be judged against that
rather than against a single lucky sample.

**A resting largest-block figure is meaningless without the client's request rate beside it**, and
this is the correction that makes the two measurements of this comparable at all. Same board, same
day, same firmware family:

| largest block | uptime | request rate | build |
|---:|---:|---|---|
| 25,588 B | 629 s | one read every 20 s | this §'s lock work (Snapshot published by pointer) |
| 16,372 B | 603 s | one read every 45-60 s | release candidate |
| 5,108 B | 812 s | one read every 4 s | release candidate |

A fivefold difference between the bottom two rows at comparable uptimes, where the variable is how
often something asked. So "the resting value drifts down with uptime" is the wrong shape for this
claim: the resting value is dominated by *request rate*, with uptime a much weaker second term, and
any figure quoted in this file, in an issue or in a commit message has to carry its rate or it
cannot be compared with another one. The 5,108 B reading is not evidence about an idle device; it is
evidence about a device being asked four times a minute.

The top row is a **third** variable and is quoted with its build for the same reason: 25,588 B held
flat over five minutes at a 20 s rate, on the build this section describes, is *higher* than the
release candidate managed at a gentler 45-60 s rate. That is consistent with removing one
whole-Snapshot allocate/free per second from the display task and a second per `/api/state`, and it
is the only evidence here that the lock work moved this number at all - but it is one board on one
afternoon, with rate and build changing together, so read it as encouraging rather than as a
measurement of the effect. A rate sweep on a single build is what would actually settle it.

What survives is that at a high enough request rate the resting value sits under the gate before any
burst at all. And the shipping web UI is, by design, the *gentle* client here - `resilientRead()`
backs off with jitter and shares one in-flight request per endpoint (§10.2) precisely so tabs cannot
amplify this - so a browser sees "busy, try again" and then its page, which is the designed
behaviour rather than a break. The device suite was harsher than the product, which is why it
reported the condition as thirty-five unrelated failures.

This is open, and it is **not** claimed as cured by §5's lock work - which removes real churn from
it (one whole-Snapshot allocate/free per second is gone, and `/api/state` no longer copies one per
request) without curing it. Nothing defragments a running heap. The device suite now states the
condition as **one named check** - "heap recovered enough for the gated reads" - rather than letting
each config round-trip report it as its own failure: the pacing there exists to stop the noise, not
to hide the condition, and if the heap does not come back that check is what fails.

**What that self-heal does NOT cover, corrected 2026-09-16.** The paragraph above used to read as
though it covered "the poller stops being useful". It does not, and the difference is the whole of
the next paragraph. It counts cycles that **complete and report failure**: the check sits in
`net_poller.cpp pollerTask()` *after* `pollOnce()` has returned, so a poller that stops completing
cycles at all - blocked inside `pollOnce`, in `HTTPClient`, or under it in lwIP - never reaches the
check, and the counter freezes at whatever it held rather than climbing. Its `else` branch is the
second half of the gap: it zeroes the tally whenever *either* condition lapses, so one cycle whose
largest block bounced back over 6 KB wipes fourteen, and fifteen *consecutive* is a demanding bar
on an oscillating heap. The task watchdog does not close it either. `CONFIG_ESP_TASK_WDT_PANIC=y`
watches IDLE0, and it fires when a task **busy-loops** and starves IDLE0 - which is exactly the
`Stream::timedRead()` case the 4 s read cap above was added for. A poller blocked on a semaphore or
a bounded socket read *yields*; IDLE0 runs, and nothing panics. So a poller that STOPS was
invisible to both defences at once. Observed on hardware on 2026-09-16: the poll heartbeat stopped
at t=68 s and never came back, the heap decayed over the next 60 s, `main.cpp loop()` caught its
`bad_alloc` and skipped frames exactly as designed, and the board finally died on the lwIP
`sys_timeout_abs` assert described above. Throughout, the self-heal counter was frozen, not
counting.

**Poller-liveness net (2026-09-16).** An additional, independent net keyed on *when a cycle last
completed with any outcome*, rather than on how many failed. `pollerTask()` stamps a monotonic
`millis()` at the end of every iteration - good poll, failed poll, caught `bad_alloc`, all the same
- and something on another task watches that stamp go stale, because a stuck poller cannot check
itself. The watcher is the LVGL display loop (`main.cpp loop()`): it is on the other core, it
already runs at ~1 Hz unconditionally, it has its own `bad_alloc` guard, and it costs no stack or
task of its own. The two alternatives were each worse - the AsyncTCP task only runs when someone
makes a request, so a display nobody is browsing would never be checked; and subscribing the poller
to the ESP-IDF task watchdog cannot work, because that watchdog has one *global* timeout (5 s here)
while a poll interval is 5-600 s, so the poller could never feed it. The check sits *outside*
`loop()`'s `try`/`catch` and allocates nothing (volatile reads, a stack buffer, `Serial.println`):
the frame the display skips because it could not allocate is precisely the frame in which the
poller is most likely to be stuck, and `Serial.printf` would `malloc` for a line this long.

The threshold is `max(active interval x 6, 5 min)`, plus 2 min of extra grace until the first cycle
of a boot has completed (`firmware/src/app/poller_liveness.h`, covered by `pio test -e native -f
test_liveness`). It is a multiple of the **active** interval, not a constant, because
`device.poll_seconds` is user-settable from 5 to 600 s (§6.1) and the failure backoff stretches the
interval to 300 s (§4.7) - a fixed "no poll for two minutes" would reboot a device that was merely
configured to poll slowly, or one backing off from a SEPTA outage.

**What the clock measures, and why it is not "a cycle completed" (corrected 2026-09-16).** The
first version of this net stamped only at the end of a cycle and justified its 5-minute floor as
"comfortably above one worst-case cycle, about 93 s". That 93 s was a *per-URL* figure used as a
per-cycle one, and a cycle fetches one TripUpdates feed, one TransitView per route and one schedule
per stop. On a network that silently **drops** packets - an ISP outage with DHCP still up, a
captive portal, heavy loss; a network that *refuses* fails in milliseconds and never gets near this
- a cycle's real cost was minutes per stop, so a device with several configured stops could take
longer than the window to finish a perfectly legitimate cycle, and the net would restart it
mid-cycle, over and over, with nothing wrong but the Wi-Fi. `config_store.h` allows eight stops and
the poller polls `cfg.stops`, not the visible subset, so the worst case was about an hour.

No window derived from the stop count can both cover that and still restart a frozen board soon
enough to matter, so the fix was to measure the right thing and to stop the cost compounding:

1. **Two stamps.** `net_poller.cpp` stamps liveness at every **fetch** the poller starts as well as
   at the end of every cycle, and the net judges the later of the two (`PollerLiveness::idle_ms`).
   A fetch in flight is evidence the poller is going round, which is the only question this net
   asks. `last_poll.since_s` in `/api/state` keeps its old meaning - seconds since a cycle
   *completed* - so nothing the owner sees changed.
2. **The retry layers no longer multiply.** `http_fetch.cpp` already spends three attempts and
   0.5 s + 1 s of backoff on a URL. Above it, `septa_source.cpp`'s `fetchPlausibleSchedule` and
   `net_poller.cpp`'s BusSchedules wrapper each retried again - and both existed for a backend that
   *answers* with the wrong service day (§4.4, NOTES.md 9), which always comes back with a real
   HTTP status. Both now stop on a transport-level failure (`FetchResult::status <= 0`, "could not
   be made at all"), which takes one stop's schedule from up to twelve URL fetches per cycle to
   one. Nothing about the wrong-service-day behaviour changed; host tests cover both halves.

The floor therefore has to clear **one fetch**, not one cycle, and one fetch is bounded:
3 attempts x (DNS + 4 s connect) + 1.5 s of backoff. The DNS term is not bounded by this firmware
and is worth naming: `HTTPClient::connect()` resolves through `NetworkClient::connect(host, ...)`,
whose `Network.hostByName()` takes no timeout at all and runs *before* `setConnectTimeout()` applies
to the socket. What bounds it is lwIP's own schedule - `DNS_MAX_RETRIES` 4 on a 1 s timer with
1/1/2/3 s between sends is ~7 s per configured server, and `DNS_MAX_SERVERS` is 3 - so ~21 s worst
case, read off lwIP's configuration rather than measured on the board. One fetch is then 76.5 s,
and the 5-minute floor is 3.9x it; `poller_liveness.h` `static_assert`s that ratio and
`test_liveness` checks it, so a future change to the retry policy breaks the build rather than
someone's wall. Detection latency at the default cadence stays five minutes, at any stop count,
deliberately. Resolving names ourselves and connecting by `IPAddress` would put the DNS bound back
under our control, and was rejected: it would send `Host: <ip>`, which SEPTA's CDN and Cloudflare
both need the real name in.

Four things are exempted, each of which would otherwise be a device that reboots itself for no
reason. **A firmware upload:** `web_server.cpp`'s `otaBusy()` is checked first, and the net also
stands down for a full window *after* an upload ends, so a stall timer earned during an OTA cannot
fire the moment the upload finishes or is aborted. A reboot mid-write is not a brick and never was
- `esp_ota_set_boot_partition()` runs inside `Update.end(true)`, so an interrupted upload leaves a
half-written *inactive* slot and the device comes back on the image it is already running - but it
throws the owner's upload away at the worst moment and looks exactly like a crash. The heap-wedge
counter in `pollerTask()` takes the same exemption for the same reason (added 2026-09-16; it had
none, and `wedged_polls` carries across an upload, so a device already near the threshold could
restart itself mid-`Update.write()`). **Setup and AP mode:** the net is armed by `startNetPoller()`, the last thing `setup()`
does, so it is off for the whole unprovisioned / captive-portal path - during which `loop()` is not
running anyway, because `connectWifiOrPortal()` does not return until Wi-Fi is up. **Boot:** the
extra grace covers `pollerTask`'s 45 s NTP wait plus the first cycle. **A slow or absent network:**
this one needs no exemption at all, and that is the point of stamping on *any* outcome and at every
fetch - a failed fetch is still a completed cycle, and a fetch still in flight is still a stamp, so
a device with no internet keeps the clock moving and only its backoff changes. Verified in the code rather than assumed: `pollOnce()` publishes a snapshot with
per-stop errors and returns a deadline on every path, including the out-of-memory one. There is
therefore no boot loop available to a device whose router is down, and even a genuine repeated
stall is bounded to roughly one restart per seven minutes by boot time plus the grace window.

The reason is visible rather than inferred. There was no restart reporting in this firmware at all
before this change, so `GET /api/state` gained `last_restart {esp, reason, detail, uptime_s}` (§7):
`esp` is `esp_reset_reason()`, and `reason`/`detail` come from a 20-byte note in RTC memory, which
survives `ESP.restart()` but not a power cycle - the right lifetime for "the last boot rebooted
itself, here is why" - written immediately before the restart and cleared the first time it is
read, so it is reported for exactly one boot and a later unrelated reset cannot inherit a stale
one. All three self-heal paths write it (`poll_stall`, `heap_wedge`, `lvgl_pool`), and the numbers that
caused the reboot go over serial first. `lvgl_pool` was added in the RC review (2026-09-16):
`lv_assert_hook.cpp` called `esp_restart()` without a note, so an exhausted LVGL pool looked
identical in `/api/state` to a deliberate reboot or an OTA - which matters most for exactly that
failure, because an arrivals page that does not fit reproduces the same crash on every boot and
what the owner actually has is a boot loop. `detail` carries the pool's free size and its
high-water mark, which together say whether it was exhausted or merely fragmented.
`last_poll.since_s` exposes the live observable the same way.

Residual risk, stated rather than hidden: if `loopTask` itself stops, nothing checks the poller -
but a board whose display loop has stopped is dead to the user anyway, and that is the failure the
LVGL assert handler and the `bad_alloc` guard above already address.

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
