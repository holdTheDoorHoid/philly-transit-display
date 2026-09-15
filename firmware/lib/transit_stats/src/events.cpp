#include "transit_stats/events.h"

#include <cstdlib>
#include <vector>

namespace transit_stats {

namespace {

// Appends `field` to `out`, quoting per RFC 4180 if it contains a comma or a double quote. CR/LF
// cannot reach here from toCsv() (sanitizeLogField strips them, see events.h's bounded-record
// contract) but the quoting rule still names them so a hand-built field can never break a line.
void appendCsvField(std::string& out, const std::string& field, bool first) {
  if (!first) out += ',';
  bool needs_quote = field.find_first_of(",\"\r\n") != std::string::npos;
  if (!needs_quote) {
    out += field;
    return;
  }
  out += '"';
  for (char c : field) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
}

std::string int64ToString(int64_t v) { return std::to_string(v); }
std::string int32ToString(int32_t v) { return std::to_string(v); }

// Splits one CSV line (RFC 4180: quoted fields may contain commas/CR/LF, "" is an escaped quote)
// into `fields`. A trailing '\r' (CRLF line endings) is stripped first. Always succeeds; a
// malformed trailing quote is tolerated by simply stopping the field at end of input.
void splitCsvLine(const char* line, size_t len, std::vector<std::string>& fields) {
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;

  size_t i = 0;
  while (true) {
    std::string field;
    if (i < len && line[i] == '"') {
      i++;  // opening quote
      while (i < len) {
        char c = line[i];
        if (c == '"') {
          if (i + 1 < len && line[i + 1] == '"') {
            field += '"';
            i += 2;
          } else {
            i++;  // closing quote
            break;
          }
        } else {
          field += c;
          i++;
        }
      }
      // Skip any (non-conformant) trailing bytes up to the next comma.
      while (i < len && line[i] != ',') i++;
    } else {
      while (i < len && line[i] != ',') {
        field += line[i];
        i++;
      }
    }
    fields.push_back(std::move(field));
    if (i < len && line[i] == ',') {
      i++;
      continue;
    }
    break;
  }
}

bool parseInt64(const std::string& s, int64_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0') return false;
  out = static_cast<int64_t>(v);
  return true;
}

bool parseInt32(const std::string& s, int32_t& out) {
  int64_t v = 0;
  if (!parseInt64(s, v)) return false;
  out = static_cast<int32_t>(v);
  return true;
}

}  // namespace

const char* toString(EventType t) {
  switch (t) {
    case EventType::Pred: return "pred";
    case EventType::Arrive: return "arrive";
    case EventType::Ghost: return "ghost";
    case EventType::NoShow: return "noshow";
    case EventType::Outage: return "outage";
    case EventType::Bike: return "bike";
  }
  return "pred";  // unreachable for a valid enum value; keeps -Wall happy without RTTI/exceptions
}

bool fromString(const std::string& s, EventType& out) {
  if (s == "pred") { out = EventType::Pred; return true; }
  if (s == "arrive") { out = EventType::Arrive; return true; }
  if (s == "ghost") { out = EventType::Ghost; return true; }
  if (s == "noshow") { out = EventType::NoShow; return true; }
  if (s == "outage") { out = EventType::Outage; return true; }
  if (s == "bike") { out = EventType::Bike; return true; }
  return false;
}

const char* seatsToken(const std::string& septa_value) {
  if (septa_value == "EMPTY") return "empty";
  if (septa_value == "MANY_SEATS_AVAILABLE") return "open";
  if (septa_value == "FEW_SEATS_AVAILABLE") return "few";
  if (septa_value == "STANDING_ROOM_ONLY") return "standing";
  if (septa_value == "CRUSHED_STANDING_ROOM_ONLY") return "packed";
  if (septa_value == "FULL") return "full";
  return "";
}

int seatsLevel(const std::string& token) {
  if (token == "empty") return 0;
  if (token == "open") return 1;
  if (token == "few") return 2;
  if (token == "standing") return 3;
  if (token == "packed") return 4;
  if (token == "full") return 5;
  return -1;
}

int csvSchemaVersion() { return 3; }

const char* csvHeader() {
  // Log schema v3 (DESIGN.md §9.1, 2026-09-15): same 21 columns as v2, with `temp` renamed to
  // `temp_c` because the column is now always Celsius. Single source of truth -- sd_logger.cpp
  // must call this rather than spell the columns out again (F24).
  return "ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,late_min,"
         "horizon_s,headway_s,note,seats,temp_c,wx,alert,bikes,ebikes,docks";
}

const char* csvHeaderV1() {
  return "ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,late_min,"
         "horizon_s,headway_s,note";
}

std::string sanitizeLogField(const std::string& field, size_t max_chars) {
  std::string out;
  out.reserve(field.size() < max_chars ? field.size() : max_chars);
  for (char c : field) {
    if (out.size() >= max_chars) break;
    const unsigned char u = static_cast<unsigned char>(c);
    // Control characters (CR/LF above all) would break the one-record-per-line contract that the
    // app's 512-byte line reader depends on; a space keeps the text readable without them.
    out += (u < 0x20 || u == 0x7F) ? ' ' : c;
  }
  return out;
}

namespace {

// Builds the 21-column row from already-sanitized text fields. Split out of toCsv() so the
// length clamp below can rebuild the line with shorter text without duplicating the column list.
std::string encodeRow(const LogEvent& ev, const std::string& trip, const std::string& vehicle,
                      const std::string& note) {
  std::string out;
  out.reserve(192);
  appendCsvField(out, int64ToString(ev.ts), true);
  appendCsvField(out, toString(ev.event), false);
  appendCsvField(out, sanitizeLogField(ev.stop_key, kMaxCsvIdChars), false);
  appendCsvField(out, sanitizeLogField(ev.route, kMaxCsvIdChars), false);
  appendCsvField(out, sanitizeLogField(ev.dir, kMaxCsvIdChars), false);
  appendCsvField(out, trip, false);
  appendCsvField(out, vehicle, false);
  appendCsvField(out, ev.scheduled_ts ? int64ToString(*ev.scheduled_ts) : std::string(), false);
  appendCsvField(out, ev.predicted_ts ? int64ToString(*ev.predicted_ts) : std::string(), false);
  appendCsvField(out, ev.actual_ts ? int64ToString(*ev.actual_ts) : std::string(), false);
  appendCsvField(out, ev.late_min ? int32ToString(*ev.late_min) : std::string(), false);
  appendCsvField(out, ev.horizon_s ? int32ToString(*ev.horizon_s) : std::string(), false);
  appendCsvField(out, ev.headway_s ? int32ToString(*ev.headway_s) : std::string(), false);
  appendCsvField(out, note, false);
  appendCsvField(out, sanitizeLogField(ev.seats, kMaxCsvIdChars), false);
  appendCsvField(out, ev.temp ? int32ToString(*ev.temp) : std::string(), false);
  appendCsvField(out, ev.wx ? int32ToString(*ev.wx) : std::string(), false);
  appendCsvField(out, ev.alert ? std::to_string(static_cast<unsigned>(*ev.alert)) : std::string(), false);
  appendCsvField(out, ev.bikes ? int32ToString(*ev.bikes) : std::string(), false);
  appendCsvField(out, ev.ebikes ? int32ToString(*ev.ebikes) : std::string(), false);
  appendCsvField(out, ev.docks ? int32ToString(*ev.docks) : std::string(), false);
  return out;
}

}  // namespace

std::string toCsv(const LogEvent& ev) {
  std::string trip = sanitizeLogField(ev.trip, kMaxCsvTextChars);
  std::string vehicle = sanitizeLogField(ev.vehicle, kMaxCsvTextChars);
  std::string note = sanitizeLogField(ev.note, kMaxCsvTextChars);

  std::string out = encodeRow(ev, trip, vehicle, note);
  // Belt and braces for the bounded-record contract (events.h): quoting can cost more than one
  // byte per character, so give back length in the order we can most afford to lose it -- free
  // text first, then the vehicle id, then the trip id. Real rows never enter this loop.
  for (int guard = 0; guard < 4 && out.size() > kMaxCsvLineBytes; ++guard) {
    const size_t over = out.size() - kMaxCsvLineBytes;
    std::string* victim = !note.empty() ? &note : (!vehicle.empty() ? &vehicle : &trip);
    if (victim->empty()) break;  // nothing left to trim; numeric columns alone cannot overflow
    victim->resize(victim->size() > over ? victim->size() - over : 0);
    out = encodeRow(ev, trip, vehicle, note);
  }
  return out;
}

bool fromCsv(const char* line, size_t len, LogEvent& ev) {
  // Reject before parsing: a blank line and an over-long (so necessarily truncated or corrupt)
  // record are both "skip me", not errors -- see the bounded-record contract in events.h. The
  // length is measured after trimming the line terminator, so a CRLF file's records are judged by
  // the same limit as an LF file's.
  size_t trimmed = len;
  while (trimmed > 0 && (line[trimmed - 1] == '\r' || line[trimmed - 1] == '\n')) trimmed--;
  if (trimmed == 0 || trimmed > kMaxCsvLineBytes) return false;

  std::vector<std::string> f;
  f.reserve(21);
  splitCsvLine(line, len, f);
  const bool is_v2 = f.size() == 21;
  if (!is_v2 && f.size() != 14) return false;

  int64_t ts = 0;
  if (!parseInt64(f[0], ts)) return false;
  EventType type;
  if (!fromString(f[1], type)) return false;

  LogEvent out;
  out.ts = ts;
  out.event = type;
  out.stop_key = f[2];
  out.route = f[3];
  out.dir = f[4];
  out.trip = f[5];
  out.vehicle = f[6];

  auto parseOptEpoch = [](const std::string& s, std::optional<transit::Epoch>& dst) {
    if (s.empty()) { dst.reset(); return true; }
    int64_t v = 0;
    if (!parseInt64(s, v)) return false;
    dst = v;
    return true;
  };
  auto parseOptI32 = [](const std::string& s, std::optional<int32_t>& dst) {
    if (s.empty()) { dst.reset(); return true; }
    int32_t v = 0;
    if (!parseInt32(s, v)) return false;
    dst = v;
    return true;
  };
  auto parseOptU8 = [](const std::string& s, std::optional<uint8_t>& dst) {
    if (s.empty()) { dst.reset(); return true; }
    int32_t v = 0;
    if (!parseInt32(s, v)) return false;
    dst = static_cast<uint8_t>(v);
    return true;
  };

  if (!parseOptEpoch(f[7], out.scheduled_ts)) return false;
  if (!parseOptEpoch(f[8], out.predicted_ts)) return false;
  if (!parseOptEpoch(f[9], out.actual_ts)) return false;
  if (!parseOptI32(f[10], out.late_min)) return false;
  if (!parseOptI32(f[11], out.horizon_s)) return false;
  if (!parseOptI32(f[12], out.headway_s)) return false;
  out.note = f[13];

  if (is_v2) {
    out.seats = f[14];
    if (!parseOptI32(f[15], out.temp)) return false;
    if (!parseOptI32(f[16], out.wx)) return false;
    if (!parseOptU8(f[17], out.alert)) return false;
    if (!parseOptI32(f[18], out.bikes)) return false;
    if (!parseOptI32(f[19], out.ebikes)) return false;
    if (!parseOptI32(f[20], out.docks)) return false;
  }
  // v1 (14-column) rows leave seats/temp/wx/alert/bikes/ebikes/docks at their default-constructed
  // (empty/unset) values -- DESIGN.md §9.1's "rows before 2026-09-14 have 14 columns" contract.

  ev = std::move(out);
  return true;
}

bool isInferenceNote(const std::string& note) {
  return note == kNoteInferred || note == kNoteLateVanish || note == kNoteUnobserved;
}

bool normalizeCsvLine(const char* line, size_t len, std::string& out) {
  // fromCsv() is the gate: it rejects blank lines, header rows, wrong column counts and unparsable
  // numbers, which is exactly the set an export should skip rather than pass through.
  LogEvent probe;
  if (!fromCsv(line, len, probe)) return false;

  std::vector<std::string> f;
  f.reserve(21);
  splitCsvLine(line, len, f);
  if (f.size() != 14 && f.size() != 21) return false;
  f.resize(21);  // a v1 row gains seven empty fields; a 21-column row is unchanged

  // Only these columns are free text. The numeric columns must NOT be touched: late_min is
  // legitimately "-3", and a leading-quote prefix there would turn a number into text.
  static const int kTextColumns[] = {2, 3, 4, 5, 6, 13, 14};
  for (int idx : kTextColumns) {
    // Strip control characters (a historical row could carry an embedded newline the old writer
    // quoted) but do NOT truncate: an export is meant to be lossless.
    f[idx] = sanitizeLogField(f[idx], kMaxCsvLineBytes);
    const char c = f[idx].empty() ? '\0' : f[idx][0];
    if (c == '=' || c == '+' || c == '-' || c == '@') f[idx].insert(f[idx].begin(), '\'');
  }

  std::string encoded;
  encoded.reserve(len + 16);
  for (size_t i = 0; i < f.size(); i++) appendCsvField(encoded, f[i], i == 0);
  out = std::move(encoded);
  return true;
}

}  // namespace transit_stats
