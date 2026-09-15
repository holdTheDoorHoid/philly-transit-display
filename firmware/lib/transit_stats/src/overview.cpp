#include "transit_stats/overview.h"

#include <cmath>

namespace transit_stats {

namespace {
double round1(double v) { return std::round(v * 10.0) / 10.0; }
// Coverage needs finer resolution than the 1-decimal display numbers: a 15-minute outage in a
// 30-day window is a real, reportable gap and must not round away to a flat 1.0.
double round4(double v) { return std::round(v * 10000.0) / 10000.0; }

uint32_t fnv1a(const std::string& s) {
  uint32_t h = 2166136261u;
  for (char c : s) {
    h ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
    h *= 16777619u;
  }
  return h ? h : 1u;  // 0 is the "empty slot" sentinel in the excluded-key tables
}
}  // namespace

OverviewAggregator::OverviewAggregator(transit::Epoch window_start, transit::Epoch window_end,
                                        const std::vector<std::string>& reserved_stop_keys,
                                        const std::vector<std::string>& reserved_bike_keys,
                                        TzHourWeekdayFn tz_fn)
    : window_start_(window_start), window_end_(window_end), tz_fn_(tz_fn) {
  // Reservations are made before a single line is read, so the currently configured stops own
  // their slots no matter what the historical rows in the window say (F28).
  size_t i = 0;
  for (const std::string& key : reserved_stop_keys) {
    if (key.empty() || i >= kMaxOverviewStops) break;
    if (findStop(key)) continue;  // duplicate in the caller's list
    stops_[i].in_use = true;
    stops_[i].reserved = true;
    stops_[i].stop_key = key;
    i++;
  }
  size_t b = 0;
  for (const std::string& key : reserved_bike_keys) {
    if (key.empty() || b >= kMaxOverviewBikeStations) break;
    bool dup = false;
    for (size_t k = 0; k < b; k++) dup = dup || bikes_[k].station_key == key;
    if (dup) continue;
    bikes_[b].in_use = true;
    bikes_[b].reserved = true;
    bikes_[b].station_key = key;
    b++;
  }
}

OverviewAggregator::OverviewAggregator(transit::Epoch window_start, transit::Epoch window_end,
                                        TzHourWeekdayFn tz_fn)
    : OverviewAggregator(window_start, window_end, std::vector<std::string>(),
                          std::vector<std::string>(), tz_fn) {}

int64_t OverviewAggregator::overlapWithWindow(transit::Epoch a, transit::Epoch b) const {
  const transit::Epoch lo = a > window_start_ ? a : window_start_;
  const transit::Epoch hi = b < window_end_ ? b : window_end_;
  return hi > lo ? static_cast<int64_t>(hi - lo) : 0;
}

void OverviewAggregator::noteExcluded(const std::string& key,
                                       uint32_t (&hashes)[kMaxExcludedKeysTracked],
                                       uint16_t& count) {
  const uint32_t h = fnv1a(key);
  for (size_t i = 0; i < kMaxExcludedKeysTracked; i++) {
    if (hashes[i] == h) return;   // already counted this key
    if (hashes[i] == 0) {
      hashes[i] = h;
      count++;
      return;
    }
  }
  // Table full: stop counting rather than double count. The JSON documents the number as a lower
  // bound for exactly this case.
}

OverviewStopStats* OverviewAggregator::findStop(const std::string& key) {
  for (auto& s : stops_) {
    if (s.in_use && s.stop_key == key) return &s;
  }
  return nullptr;
}

OverviewStopStats* OverviewAggregator::findOrCreateStop(const std::string& key) {
  if (OverviewStopStats* s = findStop(key)) return s;
  for (auto& s : stops_) {
    if (!s.in_use) {
      s = OverviewStopStats{};
      s.in_use = true;
      s.stop_key = key;
      return &s;
    }
  }
  noteExcluded(key, excluded_stop_hashes_, excluded_stops_);
  return nullptr;  // every slot taken by a reserved or earlier-seen key: report, don't hide
}

OverviewBikeStats* OverviewAggregator::findOrCreateBike(const std::string& key) {
  for (auto& b : bikes_) {
    if (b.in_use && b.station_key == key) return &b;
  }
  for (auto& b : bikes_) {
    if (!b.in_use) {
      b = OverviewBikeStats{};
      b.in_use = true;
      b.station_key = key;
      return &b;
    }
  }
  noteExcluded(key, excluded_bike_hashes_, excluded_bikes_);
  return nullptr;
}

void OverviewAggregator::feedLine(const char* line, size_t len) {
  LogEvent ev;
  if (!fromCsv(line, len, ev)) return;

  if (ev.event == EventType::Outage) {
    handleOutageRow(ev);
    return;
  }
  if (ev.ts < window_start_ || ev.ts >= window_end_) return;

  if (ev.event == EventType::Bike) {
    handleBike(ev);
  } else {
    handleStopRow(ev);
  }
}

void OverviewAggregator::handleOutageRow(const LogEvent& ev) {
  // Outside the window an outage row may only update a stop we already know about: it must not
  // spend one of the eight slots on a stop that has nothing at all inside the window.
  const bool in_window = ev.ts >= window_start_ && ev.ts < window_end_;
  OverviewStopStats* s = in_window ? findOrCreateStop(ev.stop_key) : findStop(ev.stop_key);
  if (!s) return;
  if (in_window && ev.ts > s->last_seen_ts) s->last_seen_ts = ev.ts;

  if (ev.note == kNoteOutageEnd) {
    if (s->outage_open) {
      s->outage_seconds += overlapWithWindow(s->outage_open_start, ev.ts);
      s->outage_open = false;
    }
    return;
  }
  if (ev.note.empty() && !s->outage_open) {
    s->outage_open = true;
    s->outage_open_start = ev.ts;
  }
  // note == "no_live_vehicles" is informational, never half of a pair (see tracker.cpp).
}

void OverviewAggregator::handleStopRow(const LogEvent& ev) {
  OverviewStopStats* s = findOrCreateStop(ev.stop_key);
  if (!s) return;
  if (ev.ts > s->last_seen_ts) s->last_seen_ts = ev.ts;

  switch (ev.event) {
    case EventType::Arrive:
      s->samples++;
      if (isInferenceNote(ev.note)) s->inferred++;
      if (ev.late_min.has_value()) {
        s->late_known_count++;
        s->sum_late_known += *ev.late_min;
        if (*ev.late_min >= 0 && *ev.late_min <= 5) s->on_time_count++;  // SEPTA on-time
      }
      break;
    case EventType::Ghost: s->ghost++; break;
    case EventType::NoShow: s->noshow++; break;
    case EventType::Pred:
    case EventType::Outage:
    case EventType::Bike:
      break;  // Pred only moves last_seen_ts; Outage/Bike never reach here
  }
}

void OverviewAggregator::handleBike(const LogEvent& ev) {
  OverviewBikeStats* b = findOrCreateBike(ev.stop_key);
  if (!b) return;
  if (!ev.note.empty()) b->name = ev.note;  // "name" = the note of the LAST row seen

  int hour = 0, weekday = 0;
  tz_fn_(ev.ts, hour, weekday);
  BikeHourBucket& hb = b->by_hour[hour];
  hb.n++;
  hb.sum_bikes += static_cast<uint32_t>(ev.bikes.value_or(0));
  hb.sum_ebikes += static_cast<uint32_t>(ev.ebikes.value_or(0));
  hb.sum_docks += static_cast<uint32_t>(ev.docks.value_or(0));
}

void OverviewAggregator::toJson(ArduinoJson::JsonDocument& doc) const {
  doc["days"] = static_cast<int32_t>((window_end_ - window_start_) / 86400);

  const int64_t span = static_cast<int64_t>(window_end_) - static_cast<int64_t>(window_start_);
  uint32_t total_samples = 0, total_inferred = 0;

  ArduinoJson::JsonArray stops = doc["stops"].to<ArduinoJson::JsonArray>();
  for (const auto& s : stops_) {
    if (!s.in_use) continue;
    total_samples += s.samples;
    total_inferred += s.inferred;

    ArduinoJson::JsonObject o = stops.add<ArduinoJson::JsonObject>();
    o["stop"] = s.stop_key;
    o["samples"] = s.samples;
    o["late_known"] = s.late_known_count;
    // Null, not 0: no known-lateness sample means there is no percentage, and the same for the
    // mean (F21). The device says "no data", which is true, instead of "0%", which is not.
    if (s.late_known_count) {
      o["on_time_pct"] = round1(100.0 * s.on_time_count / s.late_known_count);
      o["mean_late_min"] = round1(static_cast<double>(s.sum_late_known) / s.late_known_count);
    } else {
      o["on_time_pct"] = nullptr;
      o["mean_late_min"] = nullptr;
    }
    o["inferred"] = s.inferred;
    o["ghost"] = s.ghost;
    o["noshow"] = s.noshow;

    int64_t outage_s = s.outage_seconds;
    if (s.outage_open) outage_s += overlapWithWindow(s.outage_open_start, window_end_);
    o["outage_min"] = static_cast<int32_t>(outage_s / 60);
    double coverage = span > 0 ? 1.0 - static_cast<double>(outage_s) / static_cast<double>(span) : 0.0;
    if (coverage < 0.0) coverage = 0.0;
    if (coverage > 1.0) coverage = 1.0;
    o["coverage"] = round4(coverage);
    o["last_seen_ts"] = s.last_seen_ts;
  }

  ArduinoJson::JsonArray bikes = doc["bikes"].to<ArduinoJson::JsonArray>();
  for (const auto& b : bikes_) {
    if (!b.in_use) continue;
    ArduinoJson::JsonObject o = bikes.add<ArduinoJson::JsonObject>();
    o["station"] = b.station_key;
    o["name"] = b.name;
    uint32_t station_samples = 0;
    ArduinoJson::JsonArray by_hour = o["by_hour"].to<ArduinoJson::JsonArray>();
    for (int h = 0; h < 24; h++) {
      const BikeHourBucket& hb = b.by_hour[h];
      station_samples += hb.n;
      ArduinoJson::JsonObject ho = by_hour.add<ArduinoJson::JsonObject>();
      ho["h"] = h;
      ho["n"] = hb.n;
      ho["bikes"] = hb.n ? round1(static_cast<double>(hb.sum_bikes) / hb.n) : 0.0;
      ho["ebikes"] = hb.n ? round1(static_cast<double>(hb.sum_ebikes) / hb.n) : 0.0;
      ho["docks"] = hb.n ? round1(static_cast<double>(hb.sum_docks) / hb.n) : 0.0;
    }
    o["samples"] = station_samples;
  }

  doc["samples"] = total_samples;
  doc["inferred"] = total_inferred;
  // How many distinct keys in this window did NOT get a slot. The UI must show this ("2 older
  // stops not shown") rather than present a partial list as the whole truth (F28).
  doc["excluded_stops"] = excluded_stops_;
  doc["excluded_bikes"] = excluded_bikes_;
}

}  // namespace transit_stats
