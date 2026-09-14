#include "sd_logger.h"

#include <Arduino.h>

#ifdef BOARD_HAS_TF
#include <SD.h>
#include <SPI.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string>

namespace transit_app {

namespace {
constexpr const char *kLogDir = "/transit-log";
// DESIGN.md SS9.1's event log header.
constexpr const char *kCsvHeader = "ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,late_min,horizon_s,headway_s,note";

SdStatus g_status;

#ifdef BOARD_HAS_TF
SPIClass g_sd_spi(VSPI);
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
}  // namespace

SdStatus mountSd() {
#ifdef BOARD_HAS_TF
  g_sd_spi.begin(TF_SPI_SCLK, TF_SPI_MISO, TF_SPI_MOSI, TF_CS);
  Serial.printf("[heap] sd-pre    free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  bool began = SD.begin(TF_CS, g_sd_spi, 4000000, "/sd", 2 /* max open files */);
  Serial.printf("[heap] sd-begin  free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (!began) {
    log_w("sd_logger: SD.begin() failed (no card, or not readable)");
    g_status = SdStatus{};
    return g_status;
  }
  if (SD.cardType() == CARD_NONE) {
    log_w("sd_logger: SD card slot reports no card");
    SD.end();
    g_status = SdStatus{};
    return g_status;
  }

  if (!SD.exists(kLogDir)) {
    SD.mkdir(kLogDir);
  }

  g_status.mounted = true;
  g_status.total_bytes = SD.totalBytes();
  g_status.used_bytes = SD.usedBytes();
  g_status.free_bytes = g_status.total_bytes > g_status.used_bytes ? g_status.total_bytes - g_status.used_bytes : 0;
  log_i("sd_logger: mounted, %llu MB free of %llu MB", g_status.free_bytes / (1024ULL * 1024), g_status.total_bytes / (1024ULL * 1024));
  return g_status;
#else
  log_w("sd_logger: this board's JSON does not define BOARD_HAS_TF; no SD slot");
  g_status = SdStatus{};
  return g_status;
#endif
}

SdStatus getSdStatus() {
  return g_status;
}

uint64_t logBytes() {
#ifdef BOARD_HAS_TF
  if (!g_status.mounted) {
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
  if (!g_status.mounted) {
    return false;
  }

  // DESIGN.md SS9.1: "refuse to log when SD free space < 50 MB and show a
  // warning." Recomputed per call (not cached from mount time) since a
  // long-running device's free space only shrinks as logs accumulate.
  uint64_t total = SD.totalBytes();
  uint64_t used = SD.usedBytes();
  uint64_t free_bytes = total > used ? total - used : 0;
  g_status.total_bytes = total;
  g_status.used_bytes = used;
  g_status.free_bytes = free_bytes;
  constexpr uint64_t kMinFreeBytes = 50ULL * 1024 * 1024;
  if (free_bytes < kMinFreeBytes) {
    log_w("sd_logger: refusing to log, only %llu MB free (< 50 MB minimum)", free_bytes / (1024ULL * 1024));
    return false;
  }

  std::string path = monthPath(month);
  bool needs_header = !SD.exists(path.c_str());

  File f = SD.open(path.c_str(), FILE_APPEND);
  if (!f) {
    log_e("sd_logger: failed to open %s for append", path.c_str());
    return false;
  }
  if (needs_header) {
    f.println(kCsvHeader);
  }
  f.println(line);
  f.close();
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
  if (!g_status.mounted) return out;
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
  if (!g_status.mounted) return false;
  std::string path = logFilePath(filename);
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }
  constexpr size_t kLineBufCap = 512;
  char buf[kLineBufCap];
  bool keep_going = true;
  while (keep_going && f.available()) {
    size_t n = f.readBytesUntil('\n', buf, kLineBufCap - 1);
    if (n == 0) break;
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

File openLogFile(const std::string &filename) {
#ifdef BOARD_HAS_TF
  if (!g_status.mounted) return File();
  std::string path = logFilePath(filename);
  return SD.open(path.c_str(), FILE_READ);
#else
  (void)filename;
  return File();
#endif
}

}  // namespace transit_app
