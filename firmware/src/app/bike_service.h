// Indego bike share (DESIGN.md SS4.9, SS6 "bike"): fetches Bicycle Transit's status feed on the
// poller task every 5 minutes and keeps the configured stations for the UI and /api/state.
// lib/indego_core scans the ~400 KB body feature by feature; nothing is buffered whole.
#pragma once
#include <cstdint>
#include <vector>

#include "config_store.h"
#include "indego_core/indego.h"
#include "transit_core/source.h"

namespace transit_app {

void refreshBikes(const Config &cfg, const transit::HttpGet &http);  // once per poll cycle; no-op when not due
void invalidateBikes();                                              // config changed

// Allocates the feed scanner, whose one-feature scratch buffer is a 6,144 B contiguous block, at
// the same point in setup() as the other long-lived objects - before Wi-Fi, out of a heap that is
// still one run (DESIGN.md SS5 "the poll working set"). Returns false if it could not be had, in
// which case refreshBikes() builds one per refresh exactly as before.
bool preallocateBikeStream();

struct BikeView {
  bool enabled = false;
  uint32_t fetched_epoch = 0;  // 0 = nothing fetched yet
  std::vector<indego::Station> stations;  // in config order; a station missing from the feed keeps id/name, counts -1
};
BikeView getBikes();  // thread-safe copy

}  // namespace transit_app
