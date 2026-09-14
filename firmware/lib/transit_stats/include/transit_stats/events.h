// LogEvent: one row of the SD-card event log (DESIGN.md §9.1).
// Arduino-independent: no Arduino.h, no String, no exceptions, no RTTI.
//
// Memory: LogEvent holds seven std::string members (mostly short SEPTA ids that fit in
// small-string-optimization, typically no heap allocation) plus six std::optional<int64_t/int32_t>
// scalars. sizeof(LogEvent) is on the order of 250-300 bytes on a 64-bit host and somewhat less
// on the 32-bit ESP32 target; it is only ever used transiently (one at a time, or in a small
// std::vector<LogEvent> batch per observe() call), never persisted in bulk, so its size is not
// budget-critical the way ArrivalTracker/StatsAggregator are.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "transit_core/model.h"

namespace transit_stats {

// Kind of event emitted by ArrivalTracker. String form (see toString/fromString) matches the
// literal column values used in the CSV log and in DESIGN.md §9.1 exactly: "pred", "arrive",
// "ghost", "noshow", "outage".
enum class EventType : uint8_t { Pred, Arrive, Ghost, NoShow, Outage };

// Returns the canonical lowercase CSV token for `t` (never null, never needs freeing).
const char* toString(EventType t);

// Parses the canonical CSV token into `out`. Returns false (leaving `out` unchanged) if `s` does
// not match any known token; used both for real event columns and, deliberately, to make a CSV
// header row ("event") fail to parse so callers can skip it without special-casing it.
bool fromString(const std::string& s, EventType& out);

// One row of /transit-log/YYYY-MM.csv. Column order matches DESIGN.md §9.1 exactly:
//   ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,late_min,
//   horizon_s,headway_s,note
//
// The six optional numeric fields use std::optional so that "unknown" (empty CSV field) is
// representable distinctly from the value 0, per DESIGN's requirement that a spreadsheet be able
// to tell the two apart.
struct LogEvent {
  transit::Epoch ts = 0;          // when this log line was generated (unix seconds, UTC)
  EventType event = EventType::Pred;
  std::string stop_key;           // StopConfig::key, e.g. "17-21332"
  std::string route;               // StopConfig::route, e.g. "17"; empty if not registered
  std::string dir;                 // StopConfig::direction, e.g. "0"/"1"/"N"/"S"
  std::string trip;                // realtime or static-schedule trip id, depending on event
  std::string vehicle;              // vehicle id, may be empty
  std::optional<transit::Epoch> scheduled_ts;
  std::optional<transit::Epoch> predicted_ts;
  std::optional<transit::Epoch> actual_ts;
  std::optional<int32_t> late_min;
  std::optional<int32_t> horizon_s;
  std::optional<int32_t> headway_s;
  std::string note;                 // free text, e.g. "end" for an outage-recovery row
};

// The literal CSV header line (no trailing newline), written once per new monthly log file.
const char* csvHeader();

// Serializes `ev` as one RFC 4180 CSV line (no trailing newline / CR). Any field containing a
// comma, double quote, CR or LF is quoted with doubled internal quotes. Optional numeric fields
// that are unset are written as an empty field, never as "0".
std::string toCsv(const LogEvent& ev);

// Parses one CSV line (as found in the log file, `len` bytes starting at `line`, NOT including
// the trailing '\n'; a trailing '\r' from CRLF line endings is tolerated and stripped) into `ev`.
// Returns false, leaving `ev` unspecified, if the line does not have exactly 14 fields, if `ts`
// or `event` fail to parse, or if any non-empty optional numeric field is not a valid integer.
// A header row (whose "event" field is literally "event") always fails to parse and is safely
// skippable by the caller for that reason.
bool fromCsv(const char* line, size_t len, LogEvent& ev);

}  // namespace transit_stats
