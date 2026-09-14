#include "transit_stats/events.h"

#include <cstdlib>
#include <vector>

namespace transit_stats {

namespace {

// Appends `field` to `out`, quoting per RFC 4180 if it contains a comma, double quote, CR or LF.
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
  }
  return "pred";  // unreachable for a valid enum value; keeps -Wall happy without RTTI/exceptions
}

bool fromString(const std::string& s, EventType& out) {
  if (s == "pred") { out = EventType::Pred; return true; }
  if (s == "arrive") { out = EventType::Arrive; return true; }
  if (s == "ghost") { out = EventType::Ghost; return true; }
  if (s == "noshow") { out = EventType::NoShow; return true; }
  if (s == "outage") { out = EventType::Outage; return true; }
  return false;
}

const char* csvHeader() {
  return "ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,late_min,"
         "horizon_s,headway_s,note";
}

std::string toCsv(const LogEvent& ev) {
  std::string out;
  out.reserve(128);
  appendCsvField(out, int64ToString(ev.ts), true);
  appendCsvField(out, toString(ev.event), false);
  appendCsvField(out, ev.stop_key, false);
  appendCsvField(out, ev.route, false);
  appendCsvField(out, ev.dir, false);
  appendCsvField(out, ev.trip, false);
  appendCsvField(out, ev.vehicle, false);
  appendCsvField(out, ev.scheduled_ts ? int64ToString(*ev.scheduled_ts) : std::string(), false);
  appendCsvField(out, ev.predicted_ts ? int64ToString(*ev.predicted_ts) : std::string(), false);
  appendCsvField(out, ev.actual_ts ? int64ToString(*ev.actual_ts) : std::string(), false);
  appendCsvField(out, ev.late_min ? int32ToString(*ev.late_min) : std::string(), false);
  appendCsvField(out, ev.horizon_s ? int32ToString(*ev.horizon_s) : std::string(), false);
  appendCsvField(out, ev.headway_s ? int32ToString(*ev.headway_s) : std::string(), false);
  appendCsvField(out, ev.note, false);
  return out;
}

bool fromCsv(const char* line, size_t len, LogEvent& ev) {
  std::vector<std::string> f;
  f.reserve(14);
  splitCsvLine(line, len, f);
  if (f.size() != 14) return false;

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

  if (!parseOptEpoch(f[7], out.scheduled_ts)) return false;
  if (!parseOptEpoch(f[8], out.predicted_ts)) return false;
  if (!parseOptEpoch(f[9], out.actual_ts)) return false;
  if (!parseOptI32(f[10], out.late_min)) return false;
  if (!parseOptI32(f[11], out.horizon_s)) return false;
  if (!parseOptI32(f[12], out.headway_s)) return false;
  out.note = f[13];

  ev = std::move(out);
  return true;
}

}  // namespace transit_stats
