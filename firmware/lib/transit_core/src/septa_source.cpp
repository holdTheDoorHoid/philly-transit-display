#include "transit_core/septa_source.h"

#include <cctype>

#include "transit_core/merge.h"

namespace transit {

namespace {

constexpr const char* kBase = "https://www3.septa.org";

std::string urlEncodeSpaces(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == ' ') {
      out += "%20";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string toLower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Buffers a bounded amount of a response body via HttpGet's streaming callback, for the JSON
// endpoints (all a few KB in practice - DESIGN.md 4.3-4.6). Refuses to grow past `cap` bytes
// (aborting the transfer) so a misbehaving/misconfigured endpoint can't blow the heap; `cap` is
// generous relative to the ~1-4 KB observed real payloads specifically so a temporarily larger
// response doesn't get silently truncated-and-misparsed. This never applies to GTFS-RT
// TripUpdates, which is streamed straight into a GtfsRtStream and never buffered (see
// SeptaSource::fetchRealtime).
constexpr size_t kJsonBodyCap = 16 * 1024;

int fetchBuffered(const std::string& url, const HttpGet& http, std::vector<uint8_t>* out) {
  out->clear();
  return http(url, [&](const uint8_t* data, size_t len) {
    if (out->size() + len > kJsonBodyCap) return false;
    out->insert(out->end(), data, data + len);
    return true;
  });
}

bool isBusOrTrolley(Mode m) { return m == Mode::Bus || m == Mode::Trolley; }

void addUnique(std::vector<std::string>& v, const std::string& item) {
  if (item.empty()) return;
  for (const auto& x : v) {
    if (x == item) return;
  }
  v.push_back(item);
}

}  // namespace

std::string septaTripUpdatesUrl() {
  return std::string(kBase) + "/gtfsrt/septa-pa-us/Trip/rtTripUpdates.pb";
}

std::string septaTransitViewUrl(const std::string& route) {
  return std::string(kBase) + "/api/TransitView/index.php?route=" + urlEncodeSpaces(route);
}

std::string septaBusSchedulesUrl(const std::string& stop_id) {
  return std::string(kBase) + "/api/BusSchedules/index.php?stop_id=" + urlEncodeSpaces(stop_id);
}

std::string septaArrivalsUrl(const std::string& station, const std::string& direction) {
  std::string url = std::string(kBase) + "/api/Arrivals/index.php?station=" + urlEncodeSpaces(station);
  if (!direction.empty()) url += "&direction=" + urlEncodeSpaces(direction);
  return url;
}

std::string alertRouteIdFor(Mode mode, const std::string& route) {
  if (route.empty()) return "";
  switch (mode) {
    case Mode::Bus:
      return "bus_route_" + route;
    case Mode::Trolley:
      return "trolley_route_" + route;
    case Mode::Subway:
      return "rr_route_" + toLower(route);
    case Mode::Rail: {
      const RailLine* line = findRailLine(route);
      if (!line) return "";
      return std::string("rr_route_") + line->alert_suffix;
    }
  }
  return "";
}

std::string septaAlertsUrl(Mode mode, const std::string& route) {
  std::string rid = alertRouteIdFor(mode, route);
  if (rid.empty()) return "";
  return std::string(kBase) + "/api/Alerts/index.php?routes=" + rid;
}

int SeptaSource::fetchRealtime(GtfsRtStream& stream, HttpGet http) {
  int status = http(septaTripUpdatesUrl(),
                     [&](const uint8_t* data, size_t len) { return stream.push(data, len); });
  stream.finish();
  return status;
}

int SeptaSource::fetchTransitView(const std::string& route, std::vector<TvVehicle>* out,
                                   HttpGet http) {
  std::vector<uint8_t> body;
  int status = fetchBuffered(septaTransitViewUrl(route), http, &body);
  if (!body.empty()) {
    ParseResult<TvVehicle> r = parseTransitView(body.data(), body.size());
    if (r.ok) *out = std::move(r.items);
  }
  return status;
}

int SeptaSource::fetchSchedule(const std::string& stop_id, std::vector<SchedEntry>* out,
                                HttpGet http) {
  std::vector<uint8_t> body;
  int status = fetchBuffered(septaBusSchedulesUrl(stop_id), http, &body);
  if (!body.empty()) {
    // Parse regardless of `status`: SEPTA has been observed returning a valid-shaped body on a
    // non-200 status for this endpoint (NOTES.md) - the status code alone is not a reliable
    // "was there usable data" signal here.
    ParseResult<SchedEntry> r = parseBusSchedules(body.data(), body.size());
    if (r.ok) *out = std::move(r.items);
  }
  return status;
}

int SeptaSource::fetchAlerts(Mode mode, const std::string& route, std::vector<transit::Alert>* out,
                              HttpGet http) {
  std::string url = septaAlertsUrl(mode, route);
  if (url.empty()) return 0;
  std::vector<uint8_t> body;
  int status = fetchBuffered(url, http, &body);
  if (!body.empty()) {
    ParseResult<transit::Alert> r = parseAlerts(body.data(), body.size());
    if (r.ok) *out = std::move(r.items);
  }
  return status;
}

int SeptaSource::fetchRailArrivals(const std::string& station, std::vector<RailArrival>* out,
                                    HttpGet http) {
  std::vector<uint8_t> body;
  int status = fetchBuffered(septaArrivalsUrl(station), http, &body);
  if (!body.empty()) {
    ParseResult<RailArrival> r = parseRailArrivals(body.data(), body.size());
    if (r.ok) *out = std::move(r.items);
  }
  return status;
}

namespace {

// Earliest entry that is still upcoming (or at most a minute past), 0 if none.
Epoch earliestUpcoming(const std::vector<SchedEntry>& entries, Epoch now) {
  Epoch best = 0;
  for (const auto& e : entries) {
    if (e.scheduled < now - 60) continue;
    if (best == 0 || e.scheduled < best) best = e.scheduled;
  }
  return best;
}

}  // namespace

bool fetchPlausibleSchedule(SeptaSource& src, const std::string& stop_id, Epoch now, HttpGet http,
                            std::vector<SchedEntry>* out) {
  std::vector<SchedEntry> best;
  Epoch best_first = 0;
  for (int attempt = 0; attempt < kScheduleFetchAttempts; ++attempt) {
    std::vector<SchedEntry> fetched;
    src.fetchSchedule(stop_id, &fetched, http);
    Epoch first = earliestUpcoming(fetched, now);
    if (first == 0) continue;  // transport/parse failure, or nothing upcoming: try again
    if (best_first == 0 || first < best_first) {
      best = std::move(fetched);
      best_first = first;
    }
    if (best_first - now <= kSchedulePlausibleS) break;
  }
  if (best.empty()) return false;
  bool plausible = best_first - now <= kSchedulePlausibleS;
  *out = std::move(best);
  return plausible;
}

Snapshot pollBusStops(const std::vector<StopConfig>& configs, Epoch now, HttpGet http,
                      ScheduleCache& cache) {
  Snapshot snap;
  snap.generated = now;
  snap.last_poll_ok = true;

  SeptaSource src;

  // Union of routes/stops for the GTFS-RT filter and the TransitView fetch loop: bus/trolley
  // only, since subway has neither (NOTES.md 7a).
  std::vector<std::string> rt_routes, rt_stops;
  // Every stop_id that needs a BusSchedules lookup: bus/trolley/subway.
  std::vector<std::string> all_stop_ids;
  for (const auto& c : configs) {
    if (isBusOrTrolley(c.mode)) {
      addUnique(rt_routes, c.route);
      addUnique(rt_stops, c.stop_id);
    }
    if (isBusOrTrolley(c.mode) || c.mode == Mode::Subway) {
      addUnique(all_stop_ids, c.stop_id);
    }
  }

  std::vector<StopTimeUpdate> rt_updates;
  if (!rt_routes.empty()) {
    GtfsRtStream stream;
    stream.setRouteFilter(rt_routes);
    stream.setStopFilter(rt_stops);
    stream.onUpdate([&](const StopTimeUpdate& u) { rt_updates.push_back(u); });
    int status = src.fetchRealtime(stream, http);
    if (status != 200) {
      snap.last_poll_ok = false;
      snap.last_error = "TripUpdates HTTP " + std::to_string(status);
    }
  }

  std::vector<TvVehicle> tv_all;
  for (const auto& route : rt_routes) {
    std::vector<TvVehicle> tv;
    src.fetchTransitView(route, &tv, http);
    tv_all.insert(tv_all.end(), tv.begin(), tv.end());
  }

  struct StopSchedule {
    std::string stop_id;
    std::vector<SchedEntry> entries;
  };
  std::vector<StopSchedule> sched_by_stop;
  for (const auto& stop_id : all_stop_ids) {
    std::vector<SchedEntry> entries;
    if (!cache.get(stop_id, &entries)) {
      std::vector<SchedEntry> fetched;
      bool plausible = fetchPlausibleSchedule(src, stop_id, now, http, &fetched);
      if (!fetched.empty()) {
        if (plausible) {
          cache.put(stop_id, fetched);
        } else {
          cache.putSuspect(stop_id, fetched);
        }
        entries = std::move(fetched);
      }
    }
    sched_by_stop.push_back({stop_id, std::move(entries)});
  }
  static const std::vector<SchedEntry> kEmptySched;
  auto schedFor = [&](const std::string& stop_id) -> const std::vector<SchedEntry>& {
    for (const auto& p : sched_by_stop) {
      if (p.stop_id == stop_id) return p.entries;
    }
    return kEmptySched;
  };

  static const std::vector<StopTimeUpdate> kEmptyRt;
  static const std::vector<TvVehicle> kEmptyTv;

  for (const auto& c : configs) {
    if (c.mode != Mode::Bus && c.mode != Mode::Trolley && c.mode != Mode::Subway) continue;
    const std::vector<StopTimeUpdate>& rt_for_stop = isBusOrTrolley(c.mode) ? rt_updates : kEmptyRt;
    const std::vector<TvVehicle>& tv_for_stop = isBusOrTrolley(c.mode) ? tv_all : kEmptyTv;
    snap.stops.push_back(mergeStop(c, rt_for_stop, tv_for_stop, schedFor(c.stop_id), now));
  }

  return snap;
}

Snapshot pollRailStops(const std::vector<StopConfig>& configs, Epoch now, HttpGet http) {
  Snapshot snap;
  snap.generated = now;
  snap.last_poll_ok = true;

  SeptaSource src;

  std::vector<std::string> stations;
  for (const auto& c : configs) {
    if (c.mode == Mode::Rail) addUnique(stations, c.station);
  }

  struct StationArrivals {
    std::string station;
    std::vector<RailArrival> arrivals;
  };
  std::vector<StationArrivals> by_station;
  for (const auto& station : stations) {
    std::vector<RailArrival> arrivals;
    int status = src.fetchRailArrivals(station, &arrivals, http);
    if (status != 200 && arrivals.empty()) {
      snap.last_poll_ok = false;
      snap.last_error = "Arrivals HTTP " + std::to_string(status) + " for " + station;
    }
    by_station.push_back({station, std::move(arrivals)});
  }
  static const std::vector<RailArrival> kEmptyRail;
  auto arrivalsFor = [&](const std::string& station) -> const std::vector<RailArrival>& {
    for (const auto& p : by_station) {
      if (p.station == station) return p.arrivals;
    }
    return kEmptyRail;
  };

  for (const auto& c : configs) {
    if (c.mode != Mode::Rail) continue;
    snap.stops.push_back(mergeRail(c, arrivalsFor(c.station), now));
  }

  return snap;
}

}  // namespace transit
