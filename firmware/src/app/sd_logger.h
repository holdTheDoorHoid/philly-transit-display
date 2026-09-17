// SD mount + monthly CSV append. DESIGN.md SS5: "No tracker logic" - event
// construction (pred/arrive/ghost/noshow/outage, DESIGN.md SS9.1) is
// transit_stats::ArrivalTracker's job; this file only owns the filesystem
// mechanics.
//
// The header line is NOT spelled out here: transit_stats::csvHeader() is the single source of
// truth for it (DESIGN.md SS9.1, review finding F24). This file used to keep its own copy, which
// stayed at v1's 14 columns while the rows written under it grew to 21 - so a month's file
// promised fourteen columns and delivered twenty-one, and every reader that trusted the header
// mis-parsed it.
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
  // Write health (F26). "mounted with free space" is not the same claim as "your rows are on the
  // card": a card pulled out mid-session, a full FAT, or a short write all keep `mounted` true
  // while silently dropping data, and the device then reports logging as healthy. These three
  // make the difference visible in GET /api/state and on the device-info screen.
  uint32_t dropped_rows = 0;   // rows appendLine() could not commit, since boot
  bool last_write_ok = true;   // did the most recent append actually land?
  std::string error;           // short human text for the most recent failure, "" when fine
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
// Safe to call from any task: the status is guarded by a mutex, because the web server reads it
// from the AsyncTCP task while the poller writes it (F26).
SdStatus getSdStatus();

// Total bytes across every file under /transit-log/ (DESIGN.md SS7's
// sd.log_bytes on GET /api/state), computed by walking the directory.
// Returns 0 if the card isn't mounted.
uint64_t logBytes();

// Appends one line (no trailing newline needed) to
// /transit-log/<month>.csv, e.g. month = "2026-09". Writes transit_stats::csvHeader() first if
// the file doesn't already exist.
//
// Returns false if the SD card isn't mounted, the card is busy (only two files may be open at
// once - see acquireLogReader), free space is under the DESIGN.md SS9.1 floor, or the write was
// SHORT. A short write is the interesting case: File::println() returns the byte count it
// actually committed, and a card that has been pulled or has filled up returns a smaller number
// instead of failing, so a caller that ignores the return value writes half a row and reports
// success. Every false return increments SdStatus::dropped_rows and sets last_write_ok/error;
// callers should say so on the serial log rather than treating it as routine.
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
// grown. Returns false (having called `each` zero times) if SD isn't mounted, the card is busy,
// or the file doesn't exist - a month with no log yet is normal, not an error, so callers should
// not surface this as one.
//
// Damage containment (F25, DESIGN.md SS9.1 "Readers must skip - never abort on"): the buffer is
// transit_stats::kMaxCsvLineBytes + 1, a blank line is SKIPPED rather than ending the scan, and a
// line that fills the buffer without reaching '\n' is skipped forward to the next '\n' instead of
// being handed on as two records. One damaged record costs exactly that record.
bool streamLogLines(const std::string &filename, const std::function<bool(const char *, size_t)> &each);

// ---- one reader at a time (F26) ---------------------------------------------------------------
//
// The SD mount is opened with max_open_files = 2 (mountSd), and the poller already holds one of
// those whenever it appends a row or recomputes a stats summary. A second concurrent download
// therefore does not merely run slowly, it fails at open() and reads as "no log for that month".
// So a download takes this lease for its whole lifetime; a second one must be refused (503) by
// the caller rather than served a truncated or empty file.
//
// acquireLogReader() returns false when a download is already in flight. Every successful acquire
// must be matched by exactly one releaseLogReader() - for a chunked response that means the
// response's completion/disconnect callback, not the end of the handler.
bool acquireLogReader();
void releaseLogReader();

// A whole monthly log file, re-emitted in ONE explicit schema for download (DESIGN.md SS9.1
// "Export"). A month written across a schema change holds v1 (14-column) and v3 (21-column) rows
// side by side; handing that file over verbatim makes the receiver guess per row. LogExport runs
// every stored line through transit_stats::normalizeCsvLine(), which widens a v1 row to 21 columns
// and applies the spreadsheet formula guard, and emits transit_stats::csvHeader() once at the top.
//
// It is NOT a replacement for the raw file: the stored bytes stay exactly what the device wrote
// (a log you cannot byte-compare against the device is not a log), and the raw path is still
// available. This is the "I want to open it in a spreadsheet" path.
//
// Shape is AwsResponseFiller's: fill() writes at most `max` bytes into `buf` and returns how many;
// 0 means the export is finished, which is what ends an AsyncWebServer chunked response.
// Lines that fail to normalise (blank, header, damaged) are skipped, per SS9.1.
//
// Lifetime: construct, check ok(), then call fill() until it returns 0. The object holds one open
// File, so it must be paired with acquireLogReader()/releaseLogReader() by the caller - the
// coordinator wires that into web_server.cpp's chunked-response path.
class LogExport {
 public:
  explicit LogExport(const std::string &filename);
  ~LogExport();
  LogExport(const LogExport &) = delete;
  LogExport &operator=(const LogExport &) = delete;

  bool ok() const { return open_; }        // false: SD unmounted or no such file
  bool finished() const { return done_; }  // true once fill() has returned 0
  size_t fill(uint8_t *buf, size_t max);

 private:
  bool nextLine();  // reads one stored line into line_, false at EOF

  File file_;
  bool open_ = false;
  bool header_sent_ = false;
  bool done_ = false;
  std::string pending_;   // normalised row (plus '\n') not yet fully copied out
  size_t pending_pos_ = 0;
  // One stored line. Sized once to transit_stats::kMaxCsvLineBytes + 1 and reused, rather than a
  // 513-byte buffer on the stack of whatever task drives the chunked response (AsyncTCP's is not
  // ours to spend). `line_len_` is the bytes actually read, not line_.size().
  std::string line_;
  size_t line_len_ = 0;
};

}  // namespace transit_app
