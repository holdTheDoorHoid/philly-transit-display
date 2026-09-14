#include "transit_stats/overview.h"

#include <cmath>

namespace transit_stats {

namespace {
double round1(double v) { return std::round(v * 10.0) / 10.0; }
}  // namespace

OverviewAggregator::OverviewAggregator(transit::Epoch window_start, transit::Epoch window_end,
                                        TzHourWeekdayFn tz_fn)
    : window_start_(window_start), window_end_(window_end), tz_fn_(tz_fn) {}

OverviewStopStats* OverviewAggregator::findOrCreateStop(const std::string& key) {
  for (auto& s : stops_) {
    if (s.in_use && s.stop_key == key) return &s;
  }
  for (auto& s : stops_) {
    if (!s.in_use) {
      s = OverviewStopStats{};
      s.in_use = true;
      s.stop_key = key;
      return &s;
    }
  }
  return nullptr;  // kMaxOverviewStops slots all taken by other keys: ignore this row
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
  return nullptr;  // kMaxOverviewBikeStations slots all taken by other keys: ignore this row
}

void OverviewAggregator::feedLine(const char* line, size_t len) {
  LogEvent ev;
  if (!fromCsv(line, len, ev)) return;
  if (ev.ts < window_start_ || ev.ts >= window_end_) return;

  if (ev.event == EventType::Bike) {
    handleBike(ev);
  } else {
    handleStopRow(ev);
  }
}

void OverviewAggregator::handleStopRow(const LogEvent& ev) {
  OverviewStopStats* s = findOrCreateStop(ev.stop_key);
  if (!s) return;
  if (ev.ts > s->last_seen_ts) s->last_seen_ts = ev.ts;

  switch (ev.event) {
    case EventType::Arrive:
      s->samples++;
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
      break;  // Pred/Outage only move last_seen_ts; Bike never reaches here
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

  ArduinoJson::JsonArray stops = doc["stops"].to<ArduinoJson::JsonArray>();
  for (const auto& s : stops_) {
    if (!s.in_use) continue;
    ArduinoJson::JsonObject o = stops.add<ArduinoJson::JsonObject>();
    o["stop"] = s.stop_key;
    o["samples"] = s.samples;
    o["on_time_pct"] = round1(s.samples ? 100.0 * s.on_time_count / s.samples : 0.0);
    o["mean_late_min"] =
        round1(s.late_known_count ? static_cast<double>(s.sum_late_known) / s.late_known_count : 0.0);
    o["ghost"] = s.ghost;
    o["noshow"] = s.noshow;
    o["last_seen_ts"] = s.last_seen_ts;
  }

  ArduinoJson::JsonArray bikes = doc["bikes"].to<ArduinoJson::JsonArray>();
  for (const auto& b : bikes_) {
    if (!b.in_use) continue;
    ArduinoJson::JsonObject o = bikes.add<ArduinoJson::JsonObject>();
    o["station"] = b.station_key;
    o["name"] = b.name;
    ArduinoJson::JsonArray by_hour = o["by_hour"].to<ArduinoJson::JsonArray>();
    for (int h = 0; h < 24; h++) {
      const BikeHourBucket& hb = b.by_hour[h];
      ArduinoJson::JsonObject ho = by_hour.add<ArduinoJson::JsonObject>();
      ho["h"] = h;
      ho["n"] = hb.n;
      ho["bikes"] = hb.n ? round1(static_cast<double>(hb.sum_bikes) / hb.n) : 0.0;
      ho["ebikes"] = hb.n ? round1(static_cast<double>(hb.sum_ebikes) / hb.n) : 0.0;
      ho["docks"] = hb.n ? round1(static_cast<double>(hb.sum_docks) / hb.n) : 0.0;
    }
  }
}

}  // namespace transit_stats
