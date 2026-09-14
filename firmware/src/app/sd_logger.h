// SD mount + monthly CSV append. DESIGN.md SS5: "No tracker logic" - event
// construction (pred/arrive/ghost/noshow/outage, DESIGN.md SS9.1) is
// transit_stats::ArrivalTracker's job; this file only owns the filesystem
// mechanics.
#pragma once
#include <FS.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

struct LogFileInfo {
  std::string name;  // e.g. "2026-09.csv" - no directory prefix
  uint64_t bytes = 0;
};

// Lists the monthly log files under /transit-log/, sorted by filename (which sorts
// chronologically for "YYYY-MM.csv" names) - the shape GET /api/log/index (DESIGN.md SS7) wants.
// Empty (not an error) if SD isn't mounted or no logs exist yet.
std::vector<LogFileInfo> listLogFiles();

// Streams "/transit-log/<filename>" one CSV line at a time (no trailing '\n'; a trailing '\r' is
// left for the caller/parser to tolerate, matching transit_stats::fromCsv's own contract) to
// `each`, stopping early if `each` returns false. Reads in a fixed-size chunk internally - never
// loads the file into RAM (DESIGN.md SS5/SS9.3), regardless of how large the month's log has
// grown. Returns false (having called `each` zero times) if SD isn't mounted or the file doesn't
// exist - a month with no log yet is normal, not an error, so callers should not surface this as
// one.
bool streamLogLines(const std::string &filename, const std::function<bool(const char *, size_t)> &each);

// Opens "/transit-log/<filename>" for reading and hands back the raw File, for
// GET /api/log/<file>.csv's streamed download (AsyncWebServer's beginResponse(fs::FS&, path, ...)
// reads it incrementally itself - this function does no buffering of its own). The returned File
// evaluates false (operator bool()) if SD isn't mounted or the file doesn't exist; callers must
// still call .close() on a valid one when done.
File openLogFile(const std::string &filename);

}  // namespace transit_app
