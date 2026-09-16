# On-device test

`exhaustive_test.py` drives a running display over its HTTP API (and reads the serial console):
every endpoint, a round-trip of every setting, the validation rules, the screen logic through
`GET /api/debug/ui` and `POST /api/debug/tap` (profiles, alternatives, night page, time-to-leave,
quiet hours with wake-on-touch, page cycling), a concurrency burst, `POST /api/reboot`, and an OTA
upload of the current build. It backs the owner's config up first and restores it at the end.

```
pip install pyserial
CYD_PIN=123456 python3 firmware/test/device/exhaustive_test.py   # edit B and the serial port at the top
```

`CYD_PIN` is required: the state-changing endpoints need the device's admin PIN in an `X-Pin`
header (DESIGN.md §7/§12). Read it off the serial console at boot — `[auth] web PIN: 123456` — or
from the device info screen on the panel. The script fails immediately with that instruction if it
is unset. It also exercises the hardening itself: 401 without a PIN, the 429 lockout and its
recovery, the 421 `Host` check, an OTA with no file, and an OTA image built for a different board.
On firmware that has it, `POST /api/debug/oom` (DESIGN.md §12.1) is run too: the heap is exhausted
on purpose and the resulting `std::bad_alloc` must be caught instead of rebooting the device; older
builds that answer 404 get a `SKIP` line. Section H checks the HTTPS transport policy (DESIGN.md
§2.1) on a firmware built with `-DTRANSIT_HTTPS`: the `transport` block of `/api/state` is well
formed, `https_preferred` keeps the arrivals flowing whatever the heap gate decides, `http` freezes
the TLS counters, `https` never fetches over plain HTTP, a bogus value is rejected, and the owner's
setting is restored; a firmware without the block prints one `SKIP H` line and moves on.

Section I drives the page cycle through `POST /api/debug/page` (DESIGN.md §7) and watches LVGL's
36 KB widget pool across it: every page must actually come up, `lv_free` must never approach zero,
no build may be refused, the arrivals page must rebuild to the same size each time, and
`lv_max_used` must stop climbing after the first few cycles — a leak shows there and nowhere else.
It changes no configuration, so it has nothing to restore, and it prints one `SKIP I` line on
firmware without the endpoint. `CYD_POOL_CYCLES` sets the number of full cycles (default 20;
32 cycles were run by hand on 2026-09-16 and the high-water mark was flat from cycle 15 on).

It is not part of `pio test` (it needs the hardware and about 15 minutes). PlatformIO ignores this
directory because it has no `test_main`. 2026-09-14: 119/122 with the three remaining items being
test timing, fixed since; run it again after any change to the UI controller or the config schema.

**Read this before running it on the owner's display.** The suite changes the live configuration and
puts the device under deliberate memory pressure, and the owner's settings come back only at the
end. Since 2026-09-16 an `atexit` handler restores them even if the run dies half way through (it
did: a `KeyError` in section F ended a run early and left a test profile and the suite's stop list
on the display), and `/api/config` answering its documented 503 body is no longer mistaken for a
config. If you see `RESTORE FAILED`, `PUT test_backup_config.json` back by hand before walking away.

The run of 2026-09-16 on `cyd-3248S035R` (this branch, `next` merged, no TLS in the image) is worth
knowing about: sections A-C passed (132 checks, including the exception-pool proof
`POST /api/debug/oom` answering `caught:true` with a 12 B largest block at the throw), section H
printed its `SKIP` line as it should on a firmware without `-DTRANSIT_HTTPS`, and then sections D-F
failed in a cluster once the device settled into the 503 "low memory, retry" state - the assertions
were reading `{}`, and section E's concurrency burst rebooted it. That is the screen/web side under
memory pressure, not anything in this branch's HTTPS work, which is not compiled into that image at
all; it wants its own look with the UI rework that landed in `next`.
