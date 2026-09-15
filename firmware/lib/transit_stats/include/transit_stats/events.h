// LogEvent: one row of the SD-card event log (DESIGN.md §9.1).
// Arduino-independent: no Arduino.h, no String, no exceptions, no RTTI.
//
// Memory: LogEvent holds seven std::string members (mostly short SEPTA ids that fit in
// small-string-optimization, typically no heap allocation) plus twelve std::optional<int64_t/
// int32_t/uint8_t> scalars. sizeof(LogEvent) is on the order of 300-350 bytes on a 64-bit host
// and somewhat less on the 32-bit ESP32 target; it is only ever used transiently (one at a time,
// or in a small std::vector<LogEvent> batch per observe() call), never persisted in bulk, so its
// size is not budget-critical the way ArrivalTracker/StatsAggregator are.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "transit_core/model.h"

namespace transit_stats {

// Kind of event emitted by ArrivalTracker, plus `Bike` (log schema v2, DESIGN.md §9.1), which is
// built by the app layer (one row per Indego station per hour, not by ArrivalTracker). String
// form (see toString/fromString) matches the literal column values used in the CSV log and in
// DESIGN.md §9.1 exactly: "pred", "arrive", "ghost", "noshow", "outage", "bike".
enum class EventType : uint8_t { Pred, Arrive, Ghost, NoShow, Outage, Bike };

// ---- `note` column vocabulary -----------------------------------------------------------------
//
// Every `arrive` row is an INFERENCE, never a measured passage time: nothing in SEPTA's feeds
// says "the bus was here". The tracker only ever sees a trip stop being predicted for our stop
// and then stop being predicted, so the `note` column records HOW that particular arrival was
// inferred, and the stats layer counts it (DESIGN.md §9.2 "inferred"). Never present an `arrive`
// row's actual_ts to a user as a fact (F22).
constexpr const char* kNoteInferred = "inferred";       // vanished within ±120 s of its prediction
constexpr const char* kNoteLateVanish = "late-vanish";  // vanished well after its predicted time
constexpr const char* kNoteUnobserved = "unobserved";   // closed after a poll outage; never seen to go
// Non-inference notes that already existed (DESIGN.md §9.1).
constexpr const char* kNoteOutageEnd = "end";                    // second row of an outage pair
constexpr const char* kNoteNoLiveVehicles = "no_live_vehicles";  // noshow folded into outage

// True if `note` is one of the three inference-method markers above, i.e. this `arrive` row's
// actual_ts was derived rather than observed. Rows written before 2026-09-15 carry an empty note
// and are also inferred (they are just from before the marker existed), so callers counting
// "how many arrivals are inferred" should treat an `arrive` row as inferred regardless; this
// helper answers the narrower question "does this row say which method was used".
bool isInferenceNote(const std::string& note);

// Returns the canonical lowercase CSV token for `t` (never null, never needs freeing).
const char* toString(EventType t);

// Parses the canonical CSV token into `out`. Returns false (leaving `out` unchanged) if `s` does
// not match any known token; used both for real event columns and, deliberately, to make a CSV
// header row ("event") fail to parse so callers can skip it without special-casing it.
bool fromString(const std::string& s, EventType& out);

// Maps SEPTA's `estimated_seat_availability` string (transit::Arrival::seats, e.g.
// "FEW_SEATS_AVAILABLE") to the log's crowding token (DESIGN.md §9.1 `seats` column): one of
// "empty", "open", "few", "standing", "packed", "full", or "" for an empty/unrecognized value.
// Never null, never needs freeing.
const char* seatsToken(const std::string& septa_value);

// Maps a crowding token (as produced by seatsToken(), or read back from the CSV) to its severity
// level 0..5 in order empty=0, open=1, few=2, standing=3, packed=4, full=5; -1 if `token` is not
// one of those six (including "" for unknown).
int seatsLevel(const std::string& token);

// One row of /transit-log/YYYY-MM.csv. Column order matches DESIGN.md §9.1 exactly:
//   ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,late_min,
//   horizon_s,headway_s,note,seats,temp_c,wx,alert,bikes,ebikes,docks
//
// The optional numeric fields use std::optional so that "unknown" (empty CSV field) is
// representable distinctly from the value 0, per DESIGN's requirement that a spreadsheet be able
// to tell the two apart.
struct LogEvent {
  transit::Epoch ts = 0;          // when this log line was generated (unix seconds, UTC)
  EventType event = EventType::Pred;
  std::string stop_key;           // StopConfig::key, e.g. "17-21332"; "indego-<id>" for Bike rows
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
  std::string note;                 // free text; see the kNote* vocabulary above
  std::string seats;                // crowding token (seatsToken()); "" if unknown. pred/arrive only.
  // CELSIUS, always (log schema v3, 2026-09-15). Rows written before that date hold whatever unit
  // the device was displaying (°F on the owner's unit) and are indistinguishable by shape -- see
  // DESIGN.md §9.1. The app layer converts for display; it must never log °F into this column.
  std::optional<int32_t> temp;
  std::optional<int32_t> wx;        // WMO weather code at ts; app layer fills this in
  std::optional<uint8_t> alert;     // 0 none, 1 alert active, 2 detour active; app layer fills this in
  std::optional<int32_t> bikes;     // Bike rows only: bikes available
  std::optional<int32_t> ebikes;    // Bike rows only: e-bikes available
  std::optional<int32_t> docks;     // Bike rows only: free docks
};

// ---- bounded-record contract (F25) ------------------------------------------------------------
//
// The writer and every reader agree on one hard limit: a complete CSV record NEVER exceeds
// kMaxCsvLineBytes, and never contains a CR or LF inside a field. That is what makes a
// read-a-line-at-a-time SD reader (which cannot track quote state across a 512-byte buffer)
// correct rather than merely usually-correct: with no embedded newline, "a line" and "a record"
// are the same thing.
//
// toCsv() enforces it: every string field is passed through sanitizeLogField(), which replaces
// CR/LF/other control characters with a space and truncates to the caps below. Worst case with
// no quoting: 3*64 (note/trip/vehicle) + 4*32 (stop_key/route/dir/seats) + 12 numeric fields of
// <= 12 bytes + 20 commas = 472 bytes. Text that also needs RFC 4180 quoting can cost more than
// one byte per character, so toCsv() additionally trims note, then vehicle, then trip until the
// encoded record fits; that only ever bites on pathological input (a note that is mostly double
// quotes), never on real SEPTA ids or the notes this project writes.
// The app's SD line reader MUST use this constant rather than a private 512 of its own, with a
// buffer of at least kMaxCsvLineBytes + 1 bytes (record + terminating NUL), and must treat a line
// that filled the buffer without reaching a newline as a damaged record to SKIP -- continuing with
// the next line, never re-entering the remainder as if it were a fresh record. fromCsv() rejects
// anything longer than this for the same reason.
constexpr size_t kMaxCsvLineBytes = 512;
constexpr size_t kMaxCsvTextChars = 64;  // note, trip, vehicle
constexpr size_t kMaxCsvIdChars = 32;    // stop_key, route, dir, seats

// Returns `field` with every CR, LF, tab and other control character (< 0x20, plus DEL) replaced
// by a single space, truncated to at most `max_chars` characters. Used by toCsv() on every string
// column; exposed so the app layer can apply the same rule when it builds a LogEvent (e.g. an
// Indego station name straight out of a vendor feed) and see the value it will actually get back.
std::string sanitizeLogField(const std::string& field, size_t max_chars);

// The current log schema version (DESIGN.md §9.1). 1 = 14 columns; 2 = 21 columns with a
// device-unit `temp`; 3 = the same 21 columns with `temp_c` in Celsius (2026-09-15).
int csvSchemaVersion();

// The literal CSV header line (no trailing newline) for the CURRENT schema, written once per new
// monthly log file. This is the single source of truth for the header: no other file may spell
// the column list out again (F24 -- sd_logger.cpp used to keep its own, stale, 14-column copy).
const char* csvHeader();

// The log schema v1 header (14 columns, files created before 2026-09-14). Only useful for reading
// and normalising historical files -- never write it.
const char* csvHeaderV1();

// Serializes `ev` as one RFC 4180 CSV line (no trailing newline / CR), all 21 columns. Any field
// containing a comma or double quote is quoted with doubled internal quotes; CR/LF can no longer
// appear at all (see the bounded-record contract above). Optional numeric fields that are unset
// are written as an empty field, never as "0". The result is always <= kMaxCsvLineBytes bytes.
std::string toCsv(const LogEvent& ev);

// Parses one CSV line (as found in the log file, `len` bytes starting at `line`, NOT including
// the trailing '\n'; a trailing '\r' from CRLF line endings is tolerated and stripped) into `ev`.
// Accepts all three row shapes (DESIGN.md §9.1):
//   * v1 -- 14 columns, through `note` (rows written before 2026-09-14): parses with `seats` = ""
//     and every later optional field unset.
//   * v2 -- 21 columns, `temp` in whatever unit the device displayed (before 2026-09-15).
//   * v3 -- 21 columns, `temp_c` in Celsius. v2 and v3 rows are the same shape and are
//     deliberately NOT distinguished here: the temp value is passed through as-is, and only the
//     file's header row (or its date) says which unit it is in.
// Returns false, leaving `ev` unspecified, if the line is blank, longer than kMaxCsvLineBytes,
// does not have exactly 14 or 21 fields, if `ts` or `event` fail to parse, or if any non-empty
// optional numeric field is not a valid integer. A header row (whose "event" field is literally
// "event") always fails to parse and is safely skippable by the caller for that reason -- as is a
// blank line or a truncated/overlong record, so a reader that simply skips what fails to parse
// keeps every later record in the file (F25).
bool fromCsv(const char* line, size_t len, LogEvent& ev);

// Rewrites one historical log line into a full current-schema (v3, 21-column) row in `out`, for
// the app layer's "download a whole month in one explicit schema" path: a v1 (14-column) row gains
// seven empty fields, a 21-column row is re-emitted as-is. Returns false (leaving `out` untouched)
// for a blank line, a header row, or anything fromCsv() rejects, so the caller can skip it and
// keep streaming.
//
// Two deliberate differences from toCsv():
//   * Spreadsheet safety: a STRING field whose first character is one of = + - @ is prefixed with
//     a single quote, so Excel/Sheets/LibreOffice treat it as text instead of a formula. This
//     happens ONLY here, in the export path -- never in the stored file, which stays the exact
//     bytes the device wrote (a log you cannot diff against the device is not a log).
//   * A 21-column row's `temp` is passed through unchanged. v2 (device-unit) and v3 (Celsius)
//     rows are indistinguishable by shape, so a mixed historical month is normalised in COLUMN
//     COUNT only; DESIGN.md §9.1 records the 2026-09-15 cutover date for the unit.
bool normalizeCsvLine(const char* line, size_t len, std::string& out);

}  // namespace transit_stats
