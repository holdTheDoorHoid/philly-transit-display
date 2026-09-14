// FreeRTOS network-polling task, pinned to core 0. DESIGN.md SS5: "Network
// polling runs on core 0. Shared state is a Snapshot guarded by a mutex,
// swapped whole (never mutated in place)."
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "transit_core/model.h"

namespace transit_app {

// Poller-specific diagnostics: richer than what lives on transit::Snapshot,
// used for GET /api/state's "last_poll" field (DESIGN.md SS7) and the
// device-info screen.
struct PollStatus {
  bool has_polled = false;  // false until the first attempt completes
  bool ok = false;
  uint32_t last_poll_epoch = 0;
  int last_http_status = 0;
  size_t last_bytes = 0;
  std::string last_error;
};

// Starts the polling task. Fetches SEPTA's TransitView for route 17
// (DESIGN.md SS1, SS4.3) every `poll_seconds` via http_fetch::get(), and
// logs status, byte count, and free heap (DESIGN.md SS5's memory rules)
// each time. Must be called once, after Wi-Fi is connected.
//
// Decoding the response and merging it into a real transit::Snapshot is
// NOT implemented here - see the TODO in net_poller.cpp marking exactly
// where transit_core::merge() plugs in once it exists. Until then,
// getSnapshot() returns a Snapshot with only `generated`/`last_poll_ok`/
// `last_error` populated (empty stops/alerts); demo_data.h supplies the
// stop/arrival content shown on the UI and GET /api/state in this
// skeleton.
void startNetPoller(uint32_t poll_seconds);

// Returns a copy of the latest Snapshot, safe to call from any task.
transit::Snapshot getSnapshot();

// Returns a copy of the latest poll diagnostics, safe to call from any task.
PollStatus getPollStatus();

}  // namespace transit_app
