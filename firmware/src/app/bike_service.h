// Indego bike share (DESIGN.md SS4.9, SS6 "bike"): fetches Bicycle Transit's status feed on the
// poller task every 10 minutes and keeps the configured stations for the UI and /api/state.
// lib/indego_core scans the ~400 KB body feature by feature; nothing is buffered whole.
#pragma once
#include <cstdint>
#include <vector>

#include "config_store.h"
#include "indego_core/indego.h"
#include "transit_core/source.h"

namespace transit_app {

// `scratch`, when non-null, is the poller's shared byte buffer (transit_core PollBuffers): the
// feed scanner borrows it for its 6 KB one-feature buffer instead of taking one of its own. By the
// time this runs, the transit fetches that used the same bytes earlier in the cycle are done with
// them. Null means the scanner owns its buffer, exactly as before.
void refreshBikes(const Config &cfg, const transit::HttpGet &http,
                  std::vector<uint8_t> *scratch = nullptr);  // once per poll cycle; no-op when not due
void invalidateBikes();                                              // config changed

// Allocates the feed scanner object itself at the same point in setup() as the other long-lived
// objects. It is ~100 bytes once its 6 KB feature buffer is borrowed from the poller's shared
// scratch (see refreshBikes); what this buys is not the bytes but never constructing the scanner
// mid-cycle. Returns false if it could not be had, in which case refreshBikes() builds one per
// refresh exactly as before.
bool preallocateBikeStream();

struct BikeView {
  bool enabled = false;
  uint32_t fetched_epoch = 0;  // 0 = nothing fetched yet
  std::vector<indego::Station> stations;  // in config order; a station missing from the feed keeps id/name, counts -1
};
BikeView getBikes();  // thread-safe copy

}  // namespace transit_app
