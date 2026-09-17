#include "sd_logger.h"

#include <Arduino.h>

#ifdef BOARD_HAS_TF
#include <SD.h>
#include <SPI.h>
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "cpu_yield.h"
#include "transit_stats/events.h"
#include "ui_lock.h"

namespace transit_app {

namespace {
constexpr const char *kLogDir = "/transit-log";
// No header constant here on purpose: transit_stats::csvHeader() is the single source of truth
// (DESIGN.md SS9.1, F24). The copy that used to live here said 14 columns while the rows written
// under it had 21.

SdStatus g_status;

// g_status is written by the poller task (appendLine, mountSd) and read by the AsyncTCP task
// (GET /api/state) and the LVGL task (device-info screen). It holds a std::string, so a portMUX
// critical section is not an option - a mutex it is (F26). Held only for struct copies, never
// across an SD operation.
SemaphoreHandle_t g_status_mutex = nullptr;

SemaphoreHandle_t statusMutex() {
  if (g_status_mutex == nullptr) g_status_mutex = xSemaphoreCreateMutex();
  return g_status_mutex;
}

// 200 ms for the poller and the web task; zero for the LVGL display task, which reads this on
// every tick the device-info page is up and must never block on another task's lock (ui_lock.h,
// DESIGN.md SS5). The hold is short either way - struct copies, never an SD operation - but "short"
// is not "never", and the panic this pass fixes came from an accessor whose hold was also short.
constexpr uint32_t kSdStatusWaitMs = 200;

struct StatusLock {
  StatusLock() : held(takeShared(statusMutex(), kSdStatusWaitMs)) {}
  ~StatusLock() {
    if (held) giveShared(statusMutex());
  }
  bool held;
};

// One log download at a time (F26): see acquireLogReader() in the header for why two open files
// is not two concurrent readers. Atomic because the web server task takes it and the response's
// disconnect callback (AsyncTCP task) releases it.
std::atomic<bool> g_log_reader_busy{false};

#ifdef BOARD_HAS_TF
SPIClass g_sd_spi(VSPI);

// Cheap "is the card still there?" probe, used only after an operation has already failed. Note
// that SD.cardType() alone cannot answer it - the core caches the type from mount time and keeps
// reporting it after the card is physically pulled - so this also asks the filesystem for the
// directory we know we created, which is a real round trip to the card.
bool cardStillPresent() {
  return SD.cardType() != CARD_NONE && SD.exists(kLogDir);
}

// Records a write failure. `gone` marks the card unmounted so the device stops claiming logging
// is healthy (F26): a pulled card otherwise keeps `mounted` true, free space frozen at whatever
// it was, and every row silently dropped.
void noteWriteFailure(const char *reason, bool gone) {
  StatusLock lock;
  if (!lock.held) return;
  g_status.dropped_rows++;
  g_status.last_write_ok = false;
  g_status.error = reason;
  if (gone) {
    g_status.mounted = false;
    g_status.free_bytes = 0;
  }
}
#endif

std::string monthPath(const char *month) {
  std::string p(kLogDir);
  p += "/";
  p += month;
  p += ".csv";
  return p;
}

// filename is already a full name like "2026-09.csv" (as listLogFiles()/GET /api/log/index hand
// back) - just directory-qualify it, no month/extension gymnastics needed.
std::string logFilePath(const std::string &filename) {
  std::string p(kLogDir);
  p += "/";
  p += filename;
  return p;
}

bool isMounted() {
  StatusLock lock;
  return lock.held && g_status.mounted;
}
}  // namespace

SdStatus mountSd() {
#ifdef BOARD_HAS_TF
  g_sd_spi.begin(TF_SPI_SCLK, TF_SPI_MISO, TF_SPI_MOSI, TF_CS);
  Serial.printf("[heap] sd-pre    free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  bool began = SD.begin(TF_CS, g_sd_spi, 4000000, "/sd", 2 /* max open files */);
  Serial.printf("[heap] sd-begin  free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (!began) {
    log_w("sd_logger: SD.begin() failed (no card, or not readable)");
    StatusLock lock;
    g_status = SdStatus{};
    g_status.error = "no card";
    return g_status;
  }
  if (SD.cardType() == CARD_NONE) {
    log_w("sd_logger: SD card slot reports no card");
    SD.end();
    StatusLock lock;
    g_status = SdStatus{};
    g_status.error = "no card";
    return g_status;
  }

  if (!SD.exists(kLogDir)) {
    SD.mkdir(kLogDir);
  }

  StatusLock lock;
  g_status.mounted = true;
  g_status.last_write_ok = true;
  g_status.error.clear();
  g_status.total_bytes = SD.totalBytes();
  g_status.used_bytes = SD.usedBytes();
  g_status.free_bytes = g_status.total_bytes > g_status.used_bytes ? g_status.total_bytes - g_status.used_bytes : 0;
  log_i("sd_logger: mounted, %llu MB free of %llu MB", g_status.free_bytes / (1024ULL * 1024), g_status.total_bytes / (1024ULL * 1024));
  return g_status;
#else
  log_w("sd_logger: this board's JSON does not define BOARD_HAS_TF; no SD slot");
  StatusLock lock;
  g_status = SdStatus{};
  g_status.error = "no SD slot on this board";
  return g_status;
#endif
}

SdStatus getSdStatus() {
  static LastGood<SdStatus> ui_last;  // display task only (ui_lock.h)
  const bool ui = onDisplayTask();
  SdStatus copy;
  {
    StatusLock lock;
    if (!lock.held) {
      if (ui) {
        ui_last.miss();
        // Last frame's line, not a default SdStatus - which says "not mounted" and would flash the
        // device page's SD row to a fault the card never had.
        return ui_last.value();
      }
      return copy;
    }
    copy = g_status;
  }
  if (ui) {
    ui_last.slot() = copy;
    ui_last.hit();
  }
  return copy;
}

uint64_t logBytes() {
#ifdef BOARD_HAS_TF
  if (!isMounted()) {
    return 0;
  }
  File dir = SD.open(kLogDir);
  if (!dir || !dir.isDirectory()) {
    return 0;
  }
  uint64_t total = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      total += f.size();
    }
    f.close();
  }
  dir.close();
  return total;
#else
  return 0;
#endif
}

bool appendLine(const char *month, const char *line) {
#ifdef BOARD_HAS_TF
  if (!isMounted()) {
    return false;
  }

  // DESIGN.md SS9.1: "refuse to log when SD free space < 50 MB and show a
  // warning." Recomputed per call (not cached from mount time) since a
  // long-running device's free space only shrinks as logs accumulate.
  uint64_t total = SD.totalBytes();
  uint64_t used = SD.usedBytes();
  uint64_t free_bytes = total > used ? total - used : 0;
  {
    StatusLock lock;
    if (lock.held) {
      g_status.total_bytes = total;
      g_status.used_bytes = used;
      g_status.free_bytes = free_bytes;
    }
  }
  constexpr uint64_t kMinFreeBytes = 50ULL * 1024 * 1024;
  if (free_bytes < kMinFreeBytes) {
    log_w("sd_logger: refusing to log, only %llu MB free (< 50 MB minimum)", free_bytes / (1024ULL * 1024));
    noteWriteFailure("disk full", false);
    return false;
  }

  std::string path = monthPath(month);
  bool needs_header = !SD.exists(path.c_str());

  File f = SD.open(path.c_str(), FILE_APPEND);
  if (!f) {
    // Either the card went away or both file handles are in use (the mount allows 2, and a log
    // download holds one for its whole life - see acquireLogReader). Both drop this row; only one
    // of them means the card is gone.
    bool gone = !cardStillPresent();
    log_e("sd_logger: failed to open %s for append (%s)", path.c_str(), gone ? "card removed" : "card busy");
    noteWriteFailure(gone ? "card removed" : "card busy", gone);
    return false;
  }
  // F26: println() returns what it actually committed. A card pulled mid-write, or a full FAT,
  // returns a short count rather than failing, so ignoring this wrote half a row and called it a
  // success. Header and row are checked separately because a short header corrupts the file for
  // every later reader, not just this row.
  bool ok = true;
  if (needs_header) {
    const char *header = transit_stats::csvHeader();
    size_t want = strlen(header) + 1;  // println appends '\n'
    if (f.println(header) < want) ok = false;
  }
  if (ok) {
    size_t want = strlen(line) + 1;
    if (f.println(line) < want) ok = false;
  }
  f.close();
  if (!ok) {
    bool gone = !cardStillPresent();
    log_e("sd_logger: short write on %s (%s)", path.c_str(), gone ? "card removed" : "write failed");
    noteWriteFailure(gone ? "card removed" : "short write", gone);
    return false;
  }
  {
    StatusLock lock;
    if (lock.held) {
      g_status.last_write_ok = true;
      g_status.error.clear();
    }
  }
  return true;
#else
  (void)month;
  (void)line;
  return false;
#endif
}

std::vector<LogFileInfo> listLogFiles() {
  std::vector<LogFileInfo> out;
#ifdef BOARD_HAS_TF
  if (!isMounted()) return out;
  File dir = SD.open(kLogDir);
  if (!dir || !dir.isDirectory()) return out;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      out.push_back({f.name(), (uint64_t)f.size()});
    }
    f.close();
  }
  dir.close();
  std::sort(out.begin(), out.end(), [](const LogFileInfo &a, const LogFileInfo &b) { return a.name < b.name; });
#endif
  return out;
}

bool streamLogLines(const std::string &filename, const std::function<bool(const char *, size_t)> &each) {
#ifdef BOARD_HAS_TF
  if (!isMounted()) return false;
  std::string path = logFilePath(filename);
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) {
      f.close();
    } else if (!cardStillPresent()) {
      // The card went away under us; say so rather than letting every stats scan read as
      // "that month has no data" (F26).
      StatusLock lock;
      if (lock.held) {
        g_status.mounted = false;
        g_status.error = "card removed";
      }
    }
    return false;
  }
  // F25/DESIGN.md SS9.1: one buffer of exactly the bounded-record size the writer promises, so a
  // legal record always fits and anything that does not is, by definition, damaged.
  constexpr size_t kLineBufCap = transit_stats::kMaxCsvLineBytes + 1;
  char buf[kLineBufCap];
  bool keep_going = true;
  // DESIGN.md SS12.1 / cpu_yield.h. THE chokepoint for every CSV scan in this firmware: the stats
  // page's per-stop summaries, GET /api/stats, GET /api/stats/overview and the log export all come
  // through here, and three of those four run on the poller task, at priority 1 on core 0, where
  // IDLE0 (priority 0) is what the task watchdog checks. A month of CSV takes 5-8 s of that task
  // measured on the owner's board, with nothing in the loop ever blocking - so the watchdog's own
  // feeder never ran and the board panicked, with the backtrace landing wherever the CPU happened
  // to be (serializeJson, in the run that found this). The yield lives HERE, in the loop nobody
  // else writes, rather than at the four call sites that would each have to remember it.
  CpuYielder yielder;
  while (keep_going && f.available()) {
    yielder.tick();
    size_t before = f.position();
    size_t n = f.readBytesUntil('\n', buf, transit_stats::kMaxCsvLineBytes);
    if (n == 0) {
      // readBytesUntil() returns 0 for a BLANK LINE and at EOF alike; the file position is what
      // tells them apart (a blank line consumed its '\n', EOF consumed nothing). A blank line
      // must be skipped, never treated as the end of the file - this loop used to `break` on it,
      // which silently discarded every record after the first blank line in a month.
      if (f.position() == before) break;
      continue;
    }
    if (n == transit_stats::kMaxCsvLineBytes) {
      // The buffer filled. Either this is a legal maximum-length record whose '\n' is the very
      // next byte, or the line is longer than any record this project writes.
      int c = f.read();
      if (c == '\r') c = f.read();
      if (c != '\n' && c != -1) {
        // Damaged/foreign line: skip forward to the next '\n' rather than handing the caller two
        // fragments that each look like a record. One bad line costs exactly that line.
        while (f.available()) {
          int d = f.read();
          if (d == '\n' || d < 0) break;
        }
        continue;
      }
    }
    buf[n] = '\0';
    keep_going = each(buf, n);
  }
  f.close();
  return true;
#else
  (void)filename;
  (void)each;
  return false;
#endif
}

bool acquireLogReader() {
  bool expected = false;
  return g_log_reader_busy.compare_exchange_strong(expected, true);
}

void releaseLogReader() {
  g_log_reader_busy.store(false);
}

// ---- LogExport --------------------------------------------------------------------------------

LogExport::LogExport(const std::string &filename) {
#ifdef BOARD_HAS_TF
  if (!isMounted()) return;
  std::string path = logFilePath(filename);
  file_ = SD.open(path.c_str(), FILE_READ);
  if (!file_ || file_.isDirectory()) {
    if (file_) file_.close();
    return;
  }
  open_ = true;
  line_.resize(transit_stats::kMaxCsvLineBytes + 1);
#else
  (void)filename;
#endif
}

LogExport::~LogExport() {
#ifdef BOARD_HAS_TF
  if (file_) file_.close();
#endif
}

bool LogExport::nextLine() {
#ifdef BOARD_HAS_TF
  while (file_.available()) {
    size_t before = file_.position();
    size_t n = file_.readBytesUntil('\n', &line_[0], transit_stats::kMaxCsvLineBytes);
    if (n == 0) {
      if (file_.position() == before) return false;  // EOF, not a blank line
      continue;                                      // blank line: skip (DESIGN.md SS9.1)
    }
    if (n == transit_stats::kMaxCsvLineBytes) {
      int c = file_.read();
      if (c == '\r') c = file_.read();
      if (c != '\n' && c != -1) {
        while (file_.available()) {
          int d = file_.read();
          if (d == '\n' || d < 0) break;
        }
        continue;  // over-long record: skip whole, never split (F25)
      }
    }
    line_len_ = n;
    return true;
  }
  return false;
#else
  return false;
#endif
}

size_t LogExport::fill(uint8_t *buf, size_t max) {
  if (!open_ || done_ || max == 0) return 0;
  size_t written = 0;
  // This runs on the AsyncTCP event task as a chunked-response filler (web_server.cpp), NOT through
  // a request handler, so it is outside handleGetState()'s guarded() wrapper. normalizeCsvLine()
  // and the pending std::string allocate, and with -fexceptions on an uncaught bad_alloc here was
  // std::terminate = reboot (device suite, 2026-09-15). Catch it: return the bytes already produced
  // (the browser gets a short-but-valid prefix of the CSV) and end the stream on the next call.
  try {
  while (written < max) {
    if (pending_pos_ < pending_.size()) {
      size_t take = std::min(max - written, pending_.size() - pending_pos_);
      memcpy(buf + written, pending_.data() + pending_pos_, take);
      written += take;
      pending_pos_ += take;
      continue;
    }
    pending_.clear();
    pending_pos_ = 0;
    if (!header_sent_) {
      // Exactly one header, always the current schema, whatever the file's own first line says
      // (DESIGN.md SS9.1 "Export"): a month spanning a schema change has a v1 header on disk.
      header_sent_ = true;
      pending_ = transit_stats::csvHeader();
      pending_ += '\n';
      continue;
    }
    if (!nextLine()) {
      done_ = true;
      break;
    }
    std::string normalized;
    if (!transit_stats::normalizeCsvLine(line_.data(), line_len_, normalized)) {
      continue;  // header row, blank, or damaged: skip, keep streaming (DESIGN.md SS9.1)
    }
    pending_ = std::move(normalized);
    pending_ += '\n';
  }
  } catch (const std::bad_alloc &) {
    Serial.printf("[sd_logger] out of memory during log export; truncating the download at %u bytes\n", (unsigned)written);
    done_ = true;  // AsyncWebServer ends the chunked response when a fill returns 0 next time
  }
  return written;
}

}  // namespace transit_app
