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

FetchResult fetchBuffered(const std::string& url, const HttpGetEx& http,
                           std::vector<uint8_t>* out) {
  out->clear();
  return http(url, [&](const uint8_t* data, size_t len) {
    if (out->size() + len > kJsonBodyCap) return false;
    out->insert(out->end(), data, data + len);
    return true;
  });
}

// Turns one buffered JSON response into a FetchOutcome. Deliberately judges the BODY rather than
// the status code (NOTES.md 1: SEPTA serves good bodies under 400/501 and bad ones under 200),
// but a body that did not arrive whole is a failure regardless of what it parsed to - a truncated
// document that happens to be valid JSON is worse than no document, because it looks fine.
//
// `borrowed` says the parse target is not this call's to give away: it is the caller's long-lived
// PollBuffers::sched, whose whole purpose is to keep its capacity. The items are then COPIED out
// size-exact instead of moved, which is also the smaller contiguous request - a 12-entry schedule
// is ~1.5 KB where the parse block is a flat 3 KB. `*out` is still left untouched on every
// failure path, which fetchPlausibleSchedule() and its host tests depend on.
template <typename T>
FetchOutcome finishJsonFetch(const char* what, const FetchResult& transport,
                              ParseResult<T>* parsed, bool body_empty, std::vector<T>* out,
                              bool borrowed = false) {
  FetchOutcome o;
  o.transport = transport;
  if (body_empty) {
    o.error = transport.status == 0 ? std::string(what) + " unreachable"
                                     : std::string(what) + " empty response";
    return o;
  }
  if (!transport.complete) {
    o.error = std::string(what) + " response truncated";
    return o;
  }
  if (!parsed->ok) {
    o.error = parsed->error.empty() ? std::string(what) + " unreadable response" : parsed->error;
    return o;
  }
  if (borrowed) {
    out->assign(parsed->items.begin(), parsed->items.end());
  } else {
    *out = std::move(parsed->items);
  }
  o.ok = true;
  return o;
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

// --- PollBuffers -----------------------------------------------------------------------------

void PollBuffers::reserveAll(size_t entity_bytes, size_t retained, size_t sched_entries) {
  // reset() reserves the entity buffer; retainUpdates() reserves the retention block. Doing both
  // here means the two largest contiguous requests a poll cycle makes are paid once, by whoever
  // calls this - on the firmware, main.cpp before Wi-Fi.
  rt.reset(entity_bytes);
  rt.retainUpdates(retained, GtfsRtStream::kDefaultMaxPerStopRoute);
  body.reserve(kBodyReserve);
  sched.items.reserve(sched_entries);
}

void PollBuffers::beginCycle() {
  if (body.capacity() > kBodyReserve) {
    // A response larger than the reserve made the vector double past it. Give that block back and
    // take a fresh 4 KB one HERE, at the top of a cycle, where the largest free block is at its
    // best - rather than letting an occasional big body become resident for the life of the
    // device. swap-with-a-temporary is the only way to make a std::vector release capacity.
    std::vector<uint8_t>().swap(body);
    body.reserve(kBodyReserve);
  }
  body.clear();
  sched.items.clear();
  sched.error.clear();
  sched.dropped = 0;
  sched.ok = true;
}

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

FetchOutcome SeptaSource::fetchRealtimeEx(GtfsRtStream& stream, HttpGetEx http) {
  FetchOutcome o;
  o.transport = http(septaTripUpdatesUrl(),
                      [&](const uint8_t* data, size_t len) { return stream.push(data, len); });
  FeedStatus fs = stream.finish();

  if (o.transport.bytes == 0) {
    o.error = o.transport.status == 0 ? "TripUpdates unreachable" : "TripUpdates empty response";
    return o;
  }
  if (!o.transport.complete || fs == FeedStatus::Truncated) {
    // The two ways this shows up: the transport knows the body was cut short, or it does not but
    // the decoder ended mid-field. Either way the feed is missing entities we would have used,
    // and reporting success here is what made a half-delivered feed look like a quiet afternoon.
    o.error = "live feed truncated";
    return o;
  }
  if (fs == FeedStatus::Malformed) {
    o.error = "live feed malformed";
    return o;
  }
  o.ok = true;  // a valid feed with zero entities is a legitimate answer, not a failure
  return o;
}

FetchOutcome SeptaSource::fetchTransitViewEx(const std::string& route,
                                              std::vector<TvVehicle>* out, HttpGetEx http) {
  std::vector<uint8_t> own_body;
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->body : own_body;
  FetchResult t = fetchBuffered(septaTransitViewUrl(route), http, &body);
  ParseResult<TvVehicle> parsed;
  if (!body.empty()) parsed = parseTransitView(body.data(), body.size());
  return finishJsonFetch("TransitView", t, &parsed, body.empty(), out);
}

FetchOutcome SeptaSource::fetchScheduleEx(const std::string& stop_id,
                                           std::vector<SchedEntry>* out, HttpGetEx http) {
  std::vector<uint8_t> own_body;
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->body : own_body;
  FetchResult t = fetchBuffered(septaBusSchedulesUrl(stop_id), http, &body);
  // Parse regardless of `status`: SEPTA has been observed returning a valid-shaped body on a
  // non-200 status for this endpoint (NOTES.md) - the status code alone is not a reliable
  // "was there usable data" signal here.
  //
  // The parse target is the caller's long-lived one when there is one, so the 24 x 128 B
  // kMaxSchedEntries reserve() inside parseBusSchedulesInto() is a no-op after the first cycle.
  ParseResult<SchedEntry> own_parsed;
  ParseResult<SchedEntry>& parsed = buffers_ != nullptr ? buffers_->sched : own_parsed;
  parsed.ok = true;
  parsed.error.clear();
  parsed.dropped = 0;
  parsed.items.clear();
  if (!body.empty()) parseBusSchedulesInto(&parsed, body.data(), body.size());
  FetchOutcome o = finishJsonFetch("SEPTA schedule", t, &parsed, body.empty(), out,
                                    buffers_ != nullptr);
  if (!o.ok && o.error.empty()) o.error = "SEPTA schedule unavailable";
  return o;
}

FetchOutcome SeptaSource::fetchAlertsEx(Mode mode, const std::string& route,
                                         std::vector<transit::Alert>* out, HttpGetEx http) {
  FetchOutcome o;
  std::string url = septaAlertsUrl(mode, route);
  if (url.empty()) {
    o.error = "no alert route id for this line";
    return o;
  }
  std::vector<uint8_t> own_body;
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->body : own_body;
  FetchResult t = fetchBuffered(url, http, &body);
  ParseResult<transit::Alert> parsed;
  if (!body.empty()) parsed = parseAlerts(body.data(), body.size());
  return finishJsonFetch("Alerts", t, &parsed, body.empty(), out);
}

FetchOutcome SeptaSource::fetchRailArrivalsEx(const std::string& station,
                                               std::vector<RailArrival>* out, HttpGetEx http) {
  std::vector<uint8_t> own_body;
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->body : own_body;
  FetchResult t = fetchBuffered(septaArrivalsUrl(station), http, &body);
  ParseResult<RailArrival> parsed;
  if (!body.empty()) parsed = parseRailArrivals(body.data(), body.size());
  return finishJsonFetch("SEPTA rail arrivals", t, &parsed, body.empty(), out);
}

// --- Legacy status-code forms, on top of the Ex ones (see septa_source.h) --------------------

int SeptaSource::fetchRealtime(GtfsRtStream& stream, HttpGet http) {
  return fetchRealtimeEx(stream, adaptHttpGet(std::move(http))).transport.status;
}

int SeptaSource::fetchSchedule(const std::string& stop_id, std::vector<SchedEntry>* out,
                                HttpGet http) {
  return fetchScheduleEx(stop_id, out, adaptHttpGet(std::move(http))).transport.status;
}

int SeptaSource::fetchAlerts(Mode mode, const std::string& route, std::vector<transit::Alert>* out,
                              HttpGet http) {
  return fetchAlertsEx(mode, route, out, adaptHttpGet(std::move(http))).transport.status;
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

bool fetchPlausibleSchedule(SeptaSource& src, const std::string& stop_id, Epoch now,
                            HttpGetEx http, std::vector<SchedEntry>* out, bool* fetched_ok) {
  std::vector<SchedEntry> best;
  Epoch best_first = 0;
  bool any_ok = false;
  for (int attempt = 0; attempt < kScheduleFetchAttempts; ++attempt) {
    std::vector<SchedEntry> fetched;
    FetchOutcome o = src.fetchScheduleEx(stop_id, &fetched, http);
    // "The endpoint answered with a schedule" is tracked separately from "that schedule looked
    // plausible": an empty-but-valid answer still means the source is up, and a stop must not be
    // marked unavailable for it.
    if (o.ok) any_ok = true;
    // A TRANSPORT failure ends the loop rather than costing another round of it. status 0 (or
    // negative, where the transport reports one) means the request could not be made at all -
    // DNS, connect, or a transport that already exhausted its own attempts and backoff on this
    // exact URL. These retries exist for a SEPTA backend that ANSWERS with the wrong service day
    // (NOTES.md 9), which always comes back with a real status; repeating a dead network here
    // just multiplies it by kScheduleFetchAttempts. On a blackholing network that multiplication,
    // stacked under the firmware's own BusSchedules retry, put a single stop's schedule at up to
    // twelve URL fetches per poll cycle - minutes per stop - which is what let the liveness net
    // reboot a healthy device mid-cycle (firmware/src/app/poller_liveness.h).
    if (o.transport.status <= 0) break;
    Epoch first = earliestUpcoming(fetched, now);
    if (first == 0) continue;  // parse failure, or nothing upcoming: try again
    if (best_first == 0 || first < best_first) {
      best = std::move(fetched);
      best_first = first;
    }
    if (best_first - now <= kSchedulePlausibleS) break;
  }
  if (fetched_ok) *fetched_ok = any_ok;
  if (best.empty()) return false;
  bool plausible = best_first - now <= kSchedulePlausibleS;
  *out = std::move(best);
  return plausible;
}

bool fetchPlausibleSchedule(SeptaSource& src, const std::string& stop_id, Epoch now, HttpGet http,
                            std::vector<SchedEntry>* out, bool* fetched_ok) {
  return fetchPlausibleSchedule(src, stop_id, now, adaptHttpGet(std::move(http)), out, fetched_ok);
}

namespace {

// Rolls the per-stop outcomes up into the Snapshot-level ones: last_poll_ok means "every stop's
// required sources succeeded" (model.h), and last_error names the first thing that went wrong.
void summarize(Snapshot* snap, const std::string& preferred_error) {
  snap->last_poll_ok = true;
  for (const auto& st : snap->stops) {
    if (st.ok) continue;
    snap->last_poll_ok = false;
    if (snap->last_error.empty()) snap->last_error = st.error;
  }
  if (!snap->last_poll_ok && !preferred_error.empty()) snap->last_error = preferred_error;
}

}  // namespace

Snapshot pollBusStops(const std::vector<StopConfig>& configs, Epoch now, HttpGetEx http,
                      ScheduleCache& cache, PollBuffers* buffers) {
  Snapshot snap;
  snap.generated = now;
  snap.last_poll_ok = true;

  SeptaSource src(buffers);

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

  // The stream's own bounded buffer, not a vector of ours: every matching entity used to be
  // appended without any total limit, so a feed with many matching entities grew the heap until
  // the device died (see gtfsrt_stream.h retainUpdates()).
  // Declared out here because the merge loop below reads stream.retained() directly rather than
  // copying it into a vector of its own. A subway-only config never feeds this stream, so it is
  // built with a zero-byte entity buffer in that case and reserves nothing at all.
  //
  // With a PollBuffers the stream is the caller's, reset rather than constructed, so the 4,096 B
  // entity buffer and the ~4,600 B retention block are not asked for again (PollBuffers). The
  // local is then an empty shell - GtfsRtStream(0) reserves nothing - and costs only its own
  // ~150 bytes of stack.
  const size_t entity_bytes = rt_routes.empty() ? 0 : 4096;
  GtfsRtStream own_stream(buffers != nullptr ? 0 : entity_bytes);
  GtfsRtStream& stream = buffers != nullptr ? buffers->rt : own_stream;
  if (buffers != nullptr) stream.reset(entity_bytes);
  bool rt_ok = true;
  std::string rt_error;
  if (!rt_routes.empty()) {
    stream.setRouteFilter(rt_routes);
    stream.setStopFilter(rt_stops);
    stream.retainUpdates();
    FetchOutcome o = src.fetchRealtimeEx(stream, http);
    rt_ok = o.ok;
    if (!rt_ok) {
      rt_error = o.error.empty() ? ("TripUpdates HTTP " + std::to_string(o.transport.status))
                                 : ("TripUpdates: " + o.error);
    }
  }
  pollTrace(kPollTraceRtStream);  // 4,096 B entity buffer + ~4,800 B retained buffer are live here
  const std::vector<StopTimeUpdate>& rt_updates = stream.retained();

  struct RouteVehicles {
    std::string route;
    bool ok = true;
  };
  std::vector<RouteVehicles> tv_ok_by_route;
  std::vector<TvVehicle> tv_all;  // grows per route; a cap-sized reserve() was a ~10-20 KB block
  for (const auto& route : rt_routes) {
    std::vector<TvVehicle> tv;
    FetchOutcome o = src.fetchTransitViewEx(route, &tv, http);
    tv_ok_by_route.push_back({route, o.ok});
    tv_all.insert(tv_all.end(), tv.begin(), tv.end());
    pollTrace(kPollTraceTransitView);  // once per route
  }
  auto tvOkFor = [&](const std::string& route) {
    for (const auto& p : tv_ok_by_route) {
      if (p.route == route) return p.ok;
    }
    return true;  // no TransitView fetch was needed for this stop's route
  };

  struct StopSchedule {
    std::string stop_id;
    std::vector<SchedEntry> entries;
    bool ok = true;
  };
  std::vector<StopSchedule> sched_by_stop;
  for (const auto& stop_id : all_stop_ids) {
    std::vector<SchedEntry> entries;
    bool ok = true;
    if (cache.get(stop_id, &entries)) {
      ok = true;  // a fresh cached schedule is usable data, whatever the network is doing
    } else {
      std::vector<SchedEntry> fetched;
      bool fetched_ok = false;
      bool plausible = fetchPlausibleSchedule(src, stop_id, now, http, &fetched, &fetched_ok);
      ok = fetched_ok;
      if (!fetched.empty()) {
        if (plausible) {
          cache.put(stop_id, fetched);
        } else {
          cache.putSuspect(stop_id, fetched);
        }
        entries = std::move(fetched);
      }
    }
    sched_by_stop.push_back({stop_id, std::move(entries), ok});
    pollTrace(kPollTraceSchedStop);  // once per stop, AFTER cache.put() has taken its long-lived copy
  }
  static const std::vector<SchedEntry> kEmptySched;
  auto schedFor = [&](const std::string& stop_id) -> const std::vector<SchedEntry>& {
    for (const auto& p : sched_by_stop) {
      if (p.stop_id == stop_id) return p.entries;
    }
    return kEmptySched;
  };
  auto schedOkFor = [&](const std::string& stop_id) {
    for (const auto& p : sched_by_stop) {
      if (p.stop_id == stop_id) return p.ok;
    }
    return true;
  };

  static const std::vector<StopTimeUpdate> kEmptyRt;
  static const std::vector<TvVehicle> kEmptyTv;

  for (const auto& c : configs) {
    if (c.mode != Mode::Bus && c.mode != Mode::Trolley && c.mode != Mode::Subway) continue;
    const std::vector<StopTimeUpdate>& rt_for_stop = isBusOrTrolley(c.mode) ? rt_updates : kEmptyRt;
    const std::vector<TvVehicle>& tv_for_stop = isBusOrTrolley(c.mode) ? tv_all : kEmptyTv;
    // Each stop is told about the sources IT depends on. A subway stop is never marked down for a
    // TripUpdates outage it does not use, and a bus stop is never marked down for another
    // stop's schedule failure - mergeStop() decides what that means for this mode (merge.h).
    SourceStatus sources;
    sources.live_ok = rt_ok;
    sources.vehicles_ok = tvOkFor(c.route);
    sources.schedule_ok = schedOkFor(c.stop_id);
    snap.stops.push_back(mergeStop(c, rt_for_stop, tv_for_stop, schedFor(c.stop_id), now, sources));
  }
  pollTrace(kPollTraceMerge);  // the per-stop arrival vectors now exist; the fetch buffers still do too

  summarize(&snap, rt_error);
  return snap;
}

Snapshot pollBusStops(const std::vector<StopConfig>& configs, Epoch now, HttpGet http,
                      ScheduleCache& cache) {
  return pollBusStops(configs, now, adaptHttpGet(std::move(http)), cache);
}

Snapshot pollRailStops(const std::vector<StopConfig>& configs, Epoch now, HttpGetEx http,
                       PollBuffers* buffers) {
  Snapshot snap;
  snap.generated = now;
  snap.last_poll_ok = true;

  SeptaSource src(buffers);

  std::vector<std::string> stations;
  for (const auto& c : configs) {
    if (c.mode == Mode::Rail) addUnique(stations, c.station);
  }

  struct StationArrivals {
    std::string station;
    std::vector<RailArrival> arrivals;
    bool ok = true;
  };
  std::vector<StationArrivals> by_station;
  for (const auto& station : stations) {
    std::vector<RailArrival> arrivals;
    FetchOutcome o = src.fetchRailArrivalsEx(station, &arrivals, http);
    by_station.push_back({station, std::move(arrivals), o.ok});
  }
  static const std::vector<RailArrival> kEmptyRail;
  auto arrivalsFor = [&](const std::string& station) -> const std::vector<RailArrival>& {
    for (const auto& p : by_station) {
      if (p.station == station) return p.arrivals;
    }
    return kEmptyRail;
  };
  auto okFor = [&](const std::string& station) {
    for (const auto& p : by_station) {
      if (p.station == station) return p.ok;
    }
    return true;
  };

  for (const auto& c : configs) {
    if (c.mode != Mode::Rail) continue;
    // One station failing marks only the stops configured for that station.
    SourceStatus sources;
    sources.live_ok = okFor(c.station);
    snap.stops.push_back(mergeRail(c, arrivalsFor(c.station), now, sources));
  }

  summarize(&snap, std::string());
  return snap;
}

Snapshot pollRailStops(const std::vector<StopConfig>& configs, Epoch now, HttpGet http) {
  return pollRailStops(configs, now, adaptHttpGet(std::move(http)));
}

}  // namespace transit
