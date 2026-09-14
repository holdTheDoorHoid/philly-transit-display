// Hardcoded demo transit::Snapshot, used until transit_core actually parses
// SEPTA responses. DESIGN.md SS3/SS8 asks for the UI to be evaluable on real
// hardware before any live library is wired in; both the LVGL main screen
// and GET /api/state show this same data so they stay consistent.
#pragma once
#include "transit_core/model.h"

namespace transit_app {

// Route 17 both directions (DESIGN.md SS1's example stops), 3 arrivals
// each, with late/early/on-time/scheduled badges represented, plus one
// active alert for the footer ticker. Arrival times are computed relative
// to `now` so the screen always shows plausible "in N min" values instead
// of drifting into the past; `now` should be the current epoch (UTC) each
// time the caller wants to refresh the display.
transit::Snapshot buildDemoSnapshot(transit::Epoch now);

}  // namespace transit_app
