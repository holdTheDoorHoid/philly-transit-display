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

It is not part of `pio test` (it needs the hardware and about 15 minutes). PlatformIO ignores this
directory because it has no `test_main`. 2026-09-14: 119/122 with the three remaining items being
test timing, fixed since; run it again after any change to the UI controller or the config schema.
