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

struct BikeView {
  bool enabled = false;
  uint32_t fetched_epoch = 0;  // 0 = nothing fetched yet
  std::vector<indego::Station> stations;  // in config order; a station missing from the feed keeps id/name, counts -1
};
BikeView getBikes();  // thread-safe copy

}  // namespace transit_app
