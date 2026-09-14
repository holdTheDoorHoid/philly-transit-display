// SD mount + monthly CSV append. DESIGN.md SS5: "No tracker logic" - event
// construction (pred/arrive/ghost/noshow/outage, DESIGN.md SS9.1) is
// transit_stats::ArrivalTracker's job; this file only owns the filesystem
// mechanics.
#pragma once
#include <cstddef>
#include <cstdint>

namespace transit_app {

struct SdStatus {
  bool mounted = false;
  uint64_t total_bytes = 0;
  uint64_t used_bytes = 0;
  uint64_t free_bytes = 0;
};

// Mounts the SD card over SPI using the board's TF_* pins (see
// firmware/boards/*.json and docs/hardware.md). Safe to call with no card
// inserted or a card that fails to mount - returns SdStatus{mounted=false}
// rather than crashing (DESIGN.md SS5: "report mounted/free MB; do not
// crash without a card"). A no-op returning mounted=false on boards whose
// board JSON does not define BOARD_HAS_TF.
SdStatus mountSd();

// Returns the status from the last mountSd() call (or a default
// mounted=false SdStatus if mountSd() was never called or failed).
SdStatus getSdStatus();

// Total bytes across every file under /transit-log/ (DESIGN.md SS7's
// sd.log_bytes on GET /api/state), computed by walking the directory.
// Returns 0 if the card isn't mounted.
uint64_t logBytes();

// Appends one line (no trailing newline needed) to
// /transit-log/<month>.csv, e.g. month = "2026-09". Writes the DESIGN.md
// SS9.1 header row first if the file doesn't already exist. Returns false
// if the SD card isn't mounted or the write failed.
bool appendLine(const char *month, const char *line);

}  // namespace transit_app
