# On-device test

`exhaustive_test.py` drives a running display over its HTTP API (and reads the serial console):
every endpoint, a round-trip of every setting, the validation rules, the screen logic through
`GET /api/debug/ui` and `POST /api/debug/tap` (profiles, alternatives, night page, time-to-leave,
quiet hours with wake-on-touch, page cycling), a concurrency burst, `POST /api/reboot`, and an OTA
upload of the current build. It backs the owner's config up first and restores it at the end.

```
pip install pyserial
python3 firmware/test/device/exhaustive_test.py      # edit B and the serial port at the top
```

It is not part of `pio test` (it needs the hardware and about 15 minutes). PlatformIO ignores this
directory because it has no `test_main`. 2026-09-14: 119/122 with the three remaining items being
test timing, fixed since; run it again after any change to the UI controller or the config schema.
