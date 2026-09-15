// Picks which monthly log files (DESIGN.md SS9.1: `/transit-log/YYYY-MM.csv`) could hold a line
// for a given time window. Arduino-independent, no state, no allocation beyond the output vector.
// Added for the firmware integration's GET /api/stats and GET /api/log/index (DESIGN.md SS7):
// both need to know which files to open/list without loading anything into memory first.
#pragma once
#include <string>
#include <vector>

#include "transit_core/model.h"

namespace transit_stats {

// Every "YYYY-MM" month whose log file could contain a line timestamped in
// [window_start, window_end) local time (America/New_York, the same zone sd_logger.cpp names
// its files by), in chronological order, inclusive of both endpoints' months. window_end is
// exclusive, matching StatsAggregator's own window convention (aggregate.h). Returns an empty
// vector if window_end <= window_start.
//
// A single day's window still returns exactly one month; a window spanning a month boundary
// returns two (or more, for windows longer than a month - e.g. days=90). Callers should treat a
// missing file for one of these months as "no data that month", not an error - a brand new
// device, or one with logging just turned on, won't have earlier months at all.
std::vector<std::string> monthsInWindow(transit::Epoch window_start, transit::Epoch window_end);

// Local (America/New_York) day number for `utc` -- days since 1970-01-01 local, so two epochs
// compare equal exactly when they fall on the same local calendar day. Lives here rather than in
// aggregate.h because ArrivalTracker needs it (headway continuity must not carry across a service
// day boundary, DESIGN.md §9.2) and tracker.h deliberately does not depend on ArduinoJson, which
// aggregate.h pulls in. Uses the same deterministic DST rule as everything else in this library,
// never the host's tz database.
int64_t localServiceDayNewYork(transit::Epoch utc);

}  // namespace transit_stats
