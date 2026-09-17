// A restart at a chosen hour of the night, on by default (owner decision, 2026-09-17).
//
// WHAT IT IS AND WHAT IT IS NOT. This device has no PSRAM, a ~100-160 KB heap, and nothing that
// defragments a running one. DESIGN.md SS5 and SS12.1 record two releases of work on the per-cycle
// contiguous demand, and 0.3.2-rc1 removes every request above ~1.2 KB from a cache-hit cycle -
// and none of that is a guarantee. Fragmentation is cumulative and the trigger is still unknown:
// the owner's board rested at a 22.5 KB largest block for thirty-five minutes and was at 3,444 B
// forty minutes later, and nothing in the record says why.
//
// So this is a MITIGATION, not a fix, and it is worth saying so in the one place someone will read
// before changing it. A boot is the only defragmentation this hardware has. Taking one deliberately
// at 03:30 - when nobody is reading a transit display - costs a few seconds of uptime and buys a
// heap that starts every day in one piece. It does not excuse leaving a leak or a growing
// allocation in place, and the per-cycle log (cycle_log.h) exists precisely so the real cause can
// still be found.
//
// THE GUARDS, and why each is there:
//
//   * ONE MINUTE, ONCE. The check runs from loop() at ~1 Hz, but only acts on the transition into
//     the matching minute, so a slow minute cannot fire it twice.
//   * UPTIME > 1 HOUR. Without it, a board that boots at 03:29 restarts at 03:30, boots again, and
//     restarts the next night at best - or, if the clock steps around the boundary, loops. An hour
//     is far longer than any plausible boot-plus-NTP sequence.
//   * A SANE CLOCK. Before NTP the local time is 1970 and "03:30" would match at a moment that has
//     nothing to do with 03:30.
//   * NOT DURING AN OTA. Restarting mid-Update.write() throws the owner's upload away at the worst
//     possible moment, exactly as in the wedge and liveness nets (wedge_policy.h, main.cpp).
//
// The decision is pure so it can be host-tested (`pio test -e native -f test_nightly`), the same
// shape as poller_liveness.h, proxy_queue.h, heap_reserve.h, admission.h and wedge_policy.h. The
// clock reading and the restart itself live in main.cpp.
#pragma once
#include <cstdint>

namespace transit_app {

// A board that has just come up must not restart again. One hour is far more than boot + Wi-Fi +
// NTP + the first poll, and far less than the gap between two nights.
constexpr uint32_t kNightlyMinUptimeS = 3600;

// Whether THIS minute is the one to restart in.
//
//   enabled          device.nightly_restart.enabled
//   want_minute      device.nightly_restart.time as minutes since local midnight, -1 if unset or
//                    unparseable (daypart::parseClock's answer)
//   now_minute       the local wall clock as minutes since midnight, -1 if the clock is not sane
//   last_minute      the now_minute this check last ran at, -1 before the first run. The caller
//                    stores what this function is given and passes it back, which is what makes
//                    "once a minute" a property of the rule rather than of the caller's loop.
//   uptime_s         seconds since boot
//   ota_busy         a firmware upload is in progress
constexpr bool shouldRestartNightly(bool enabled, int want_minute, int now_minute, int last_minute,
                                     uint32_t uptime_s, bool ota_busy) {
  if (!enabled || ota_busy) return false;
  if (want_minute < 0 || now_minute < 0) return false;   // unset, or no sane clock
  if (now_minute == last_minute) return false;           // already considered this minute
  if (uptime_s <= kNightlyMinUptimeS) return false;      // too soon after boot to be trusted
  return now_minute == want_minute;
}

}  // namespace transit_app
