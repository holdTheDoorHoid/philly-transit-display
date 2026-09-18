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

// `buffers` may be null (every non-firmware caller): then this is exactly the old behaviour, an
// owned vector grown by doubling. With one, growth past the current reservation is taken in ONE
// rounded step rather than by std::vector's 2x rule, and recorded - see PollBuffers::beginCycle()
// for why the doubling mattered.
constexpr size_t kBodyGrowStep = 1024;

FetchResult fetchBuffered(const std::string& url, const HttpGetEx& http,
                           std::vector<uint8_t>* out, PollBuffers* buffers) {
  out->clear();
  return http(url, [&](const uint8_t* data, size_t len) {
    const size_t want = out->size() + len;
    if (want > kJsonBodyCap) return false;
    if (buffers != nullptr) {
      if (want > out->capacity()) {
        // Round up to the next kBodyGrowStep and reserve exactly that: one reallocation to a size
        // the body needs, instead of a doubling to a size nothing asked for.
        const size_t rounded = ((want + kBodyGrowStep - 1) / kBodyGrowStep) * kBodyGrowStep;
        out->reserve(rounded > kJsonBodyCap ? kJsonBodyCap : rounded);
        buffers->scratch_grows++;
      }
      buffers->noteBodyBytes(want);
    }
    out->insert(out->end(), data, data + len);
    return true;
  });
}

// Turns one buffered JSON response into a FetchOutcome. Deliberately judges the BODY rather than
// the status code (NOTES.md 1: SEPTA serves good bodies under 400/501 and bad ones under 200),
// but a body that did not arrive whole is a failure regardless of what it parsed to - a truncated
// document that happens to be valid JSON is worse than no document, because it looks fine.
//
template <typename T>
FetchOutcome finishJsonFetch(const char* what, const FetchResult& transport,
                              ParseResult<T>* parsed, bool body_empty, std::vector<T>* out) {
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
  *out = std::move(parsed->items);
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

void PollBuffers::reserveAll(size_t scratch_bytes) {
  scratch_reserve = scratch_bytes;
  scratch.reserve(scratch_bytes);
  // The TransitView list is sized at the parser's own cap, which since 0.3.2-rc3 is 16 slots
  // (2,816 B) and not 32 (5,632 B). What made 32 necessary was that the parse kept every vehicle
  // on the route; the trip-id filter keeps only the ones a configured stop's retained realtime
  // updates can join to, and that is bounded by the updates, not by how busy the route is.
  tv.reserve(kMaxTvVehicles);
}

void PollBuffers::reserveRetention(size_t pairs) {
  const size_t want = pairs * kRetainedPerPair;
  if (want > retained.capacity()) retained.reserve(want);
}

void PollBuffers::noteBodyBytes(size_t bytes) {
  if (bytes > scratch_max_bytes) scratch_max_bytes = (uint32_t)bytes;
}

void PollBuffers::beginCycle(size_t free8_at_poll_start) {
  // THE CHURN THIS AVOIDS (0.3.2-rc1). The scratch is reserved at kScratchReserve, but
  // kJsonBodyCap is 16 KB, so a body larger than the reservation used to make the vector DOUBLE
  // past it (a 12,288 B contiguous request mid-cycle) and then this function gave that block back
  // and took a fresh 6,144 B one - every single cycle, for as long as that endpoint kept answering
  // big. Grow, shrink, grow, shrink: exactly the fragmentation this whole pass is fighting, and
  // invisible because nothing recorded how big a body had ever been.
  //
  // So the reservation RATCHETS instead. A body that goes past it raises it (fetchBuffered()
  // reserves the rounded-up size in one step, so the vector never doubles), and the bigger block
  // is then kept - one reallocation for the life of the boot rather than one per cycle. The
  // ratchet is bounded by kScratchMaxReserve so a pathological response cannot become resident:
  // above that, the old give-it-back behaviour still applies.
  //
  // scratch_max_bytes on GET /api/debug/ui is what says whether kScratchReserve should simply be
  // raised in the source, which is better than either of these behaviours and needs a measurement
  // this firmware could not previously take.
  if (scratch.capacity() > scratch_reserve) {
    // TWO conditions since 0.3.2-rc2, and the second one is the lesson of rc1. Keeping the bigger
    // block is only the right answer on a heap that can spare it; measured on the owner's board,
    // rc1 ratcheted the reservation past 6,144 B within four minutes while resting free8 was
    // 22-23 KB, which is precisely when growing the resident footprint is the last thing wanted.
    // On a struggling heap the block is handed back and the next big body simply pays for it
    // again - one request per oversized cycle, which is the old cost and the affordable one.
    if (scratch.capacity() <= kScratchMaxReserve && free8_at_poll_start >= kScratchRatchetMinFree8) {
      scratch_reserve = scratch.capacity();  // keep it: the ratchet
    } else {
      // swap-with-a-temporary is the only way to make a std::vector release capacity.
      std::vector<uint8_t>().swap(scratch);
      scratch.reserve(scratch_reserve);
    }
  }
  scratch.clear();
  // The retained updates are dead the moment a cycle ends - the merge loop that reads them has
  // returned - so their identifier strings are dropped here rather than held until the next
  // cycle's retainUpdates(). The block itself stays: that is the whole point.
  retained.clear();
  tv.clear();
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
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->scratch : own_body;
  FetchResult t = fetchBuffered(septaTransitViewUrl(route), http, &body, buffers_);
  ParseResult<TvVehicle> parsed;
  if (!body.empty()) parsed = parseTransitView(body.data(), body.size());
  return finishJsonFetch("TransitView", t, &parsed, body.empty(), out);
}

FetchOutcome SeptaSource::fetchTransitViewAppendEx(const std::string& route,
                                                    std::vector<TvVehicle>* out, HttpGetEx http,
                                                    const TvFilter& filter) {
  std::vector<uint8_t> own_body;
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->scratch : own_body;
  FetchResult t = fetchBuffered(septaTransitViewUrl(route), http, &body, buffers_);

  // The destination's storage IS the parse block for the duration: swapped in, parsed into,
  // swapped back. No second vector is ever constructed, so nothing contiguous is asked for.
  ParseResult<TvVehicle> parsed;
  parsed.items.swap(*out);
  const size_t before = parsed.items.size();
  if (!body.empty()) parseTransitViewAppend(&parsed, body.data(), body.size(), filter);
  // Relevant vehicles that did not fit kMaxTvVehicles. Accumulated since boot rather than per
  // cycle, because the question it answers - "is 16 slots enough for this config?" - is not a
  // question about one cycle.
  //
  // ONLY WHEN A FILTER IS INSTALLED, and this is not a detail. The one caller that brings no
  // filter is the firmware's route-liveness refresh (net_poller.cpp refreshRouteLiveness), which
  // fetches a whole route's fleet for the single question `!tv.empty()`. A rush-hour Route 17
  // carries 20-30 vehicles, so an unfiltered fetch overflows a 16-slot cap by design and counting
  // it would make tv_dropped climb on every liveness refresh - reading "the cap is too small"
  // when nothing anybody wanted was lost. The metric means "a vehicle a configured stop could
  // have used did not fit", and that only has a meaning on a filtered call.
  if (buffers_ != nullptr && filter.active() && parsed.dropped > 0) buffers_->tv_dropped += parsed.dropped;

  FetchOutcome o;
  o.transport = t;
  if (body.empty()) {
    o.error = t.status == 0 ? "TransitView unreachable" : "TransitView empty response";
  } else if (!t.complete) {
    o.error = "TransitView response truncated";
  } else if (!parsed.ok) {
    o.error = parsed.error.empty() ? "TransitView unreadable response" : parsed.error;
  } else {
    o.ok = true;
  }
  // "A failed fetch leaves *out untouched" is the contract finishJsonFetch() enforces for the
  // replacing form; here it means dropping anything this route appended before it went wrong.
  if (!o.ok && parsed.items.size() > before) parsed.items.resize(before);
  parsed.items.swap(*out);  // always hand the block back, on every path
  return o;
}

FetchOutcome SeptaSource::fetchScheduleEx(const std::string& stop_id,
                                           std::vector<SchedEntry>* out, HttpGetEx http) {
  std::vector<uint8_t> own_body;
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->scratch : own_body;
  FetchResult t = fetchBuffered(septaBusSchedulesUrl(stop_id), http, &body, buffers_);
  // Parse regardless of `status`: SEPTA has been observed returning a valid-shaped body on a
  // non-200 status for this endpoint (NOTES.md) - the status code alone is not a reliable
  // "was there usable data" signal here.
  ParseResult<SchedEntry> parsed;
  if (!body.empty()) parseBusSchedulesInto(&parsed, body.data(), body.size());
  FetchOutcome o = finishJsonFetch("SEPTA schedule", t, &parsed, body.empty(), out);
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
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->scratch : own_body;
  FetchResult t = fetchBuffered(url, http, &body, buffers_);
  ParseResult<transit::Alert> parsed;
  if (!body.empty()) parsed = parseAlerts(body.data(), body.size());
  return finishJsonFetch("Alerts", t, &parsed, body.empty(), out);
}

FetchOutcome SeptaSource::fetchRailArrivalsEx(const std::string& station,
                                               std::vector<RailArrival>* out, HttpGetEx http) {
  std::vector<uint8_t> own_body;
  std::vector<uint8_t>& body = buffers_ != nullptr ? buffers_->scratch : own_body;
  FetchResult t = fetchBuffered(septaArrivalsUrl(station), http, &body, buffers_);
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
  // ONE `fetched` for every attempt, not one per attempt. This loop is the outer half of a nested
  // retry - the firmware's own BusSchedules branch spends up to four attempts inside each of these
  // three - and it is re-entered every two minutes for as long as SEPTA's stale backends keep
  // answering with the next service day (net_poller.cpp kBusSchedulesSuspectRefreshMs). A vector
  // declared inside the loop meant a fresh allocate-and-free of a whole schedule on every one of
  // those attempts, while the GTFS-RT entity and retention buffers stayed live: the exact shape
  // that takes a largest free block from 23.5 KB to 11.7 KB in a few minutes (DESIGN.md SS5, "the
  // poll working set"). clear() keeps the capacity, and fetchScheduleEx() leaves it untouched on a
  // failure, so an attempt that fails still reads as "nothing fetched" exactly as before.
  std::vector<SchedEntry> fetched;
  for (int attempt = 0; attempt < kScheduleFetchAttempts; ++attempt) {
    fetched.clear();
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
      // swap, not move-assign: `best` takes this answer and `fetched` takes whatever block `best`
      // was holding, which the clear() above then reuses for the next attempt instead of freeing
      // it and allocating again. The observable result is identical - `fetched` is cleared before
      // it is next written, and `best` is moved into *out below.
      best.swap(fetched);
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
  // With a PollBuffers the entity buffer is BORROWED from the shared scratch rather than
  // allocated here (PollBuffers): it is live only until finish(), so the same bytes go on to serve
  // every buffered JSON response of this cycle and then the Indego scanner. The stream object
  // itself is still a local - it is ~150 bytes of stack once it owns no buffer.
  // How many (stop, route) pairs the GTFS-RT retention block actually has to hold. GtfsRtStream
  // caps each pair at kDefaultMaxPerStopRoute, so this is the exact number of slots that can ever
  // be occupied - sizing by it rather than by the 32-slot default cap is what makes the block
  // 2.4 KB for the owner's two Route 17 stops instead of 4.9 KB (0.3.2-rc1, PollBuffers).
  size_t rt_pairs = 0;
  for (const auto& c : configs) {
    if (isBusOrTrolley(c.mode)) ++rt_pairs;
  }
  size_t retain_total = rt_pairs * GtfsRtStream::kDefaultMaxPerStopRoute;
  if (retain_total > GtfsRtStream::kDefaultMaxRetainedUpdates) {
    retain_total = GtfsRtStream::kDefaultMaxRetainedUpdates;  // the cap this device has always had
  }
  if (retain_total == 0) retain_total = GtfsRtStream::kDefaultMaxPerStopRoute;

  const size_t entity_bytes = rt_routes.empty() ? 0 : 4096;
  GtfsRtStream stream(buffers != nullptr ? 0 : entity_bytes);
  if (buffers != nullptr && entity_bytes > 0) {
    stream.setEntityBuffer(&buffers->scratch);
    // The retention block is the OTHER per-cycle contiguous request this stage used to make, and
    // the one that failed on the owner's board once the largest free block reached 3,444 B. Lent,
    // not allocated: retainUpdates()' reserve() below is a no-op from the second cycle on.
    buffers->reserveRetention(rt_pairs);
    stream.setRetentionBuffer(&buffers->retained);
    stream.reset(entity_bytes);
  }
  bool rt_ok = true;
  std::string rt_error;
  if (!rt_routes.empty()) {
    stream.setRouteFilter(rt_routes);
    stream.setStopFilter(rt_stops);
    stream.retainUpdates(retain_total, GtfsRtStream::kDefaultMaxPerStopRoute);
    FetchOutcome o = src.fetchRealtimeEx(stream, http);
    rt_ok = o.ok;
    if (!rt_ok) {
      rt_error = o.error.empty() ? ("TripUpdates HTTP " + std::to_string(o.transport.status))
                                 : ("TripUpdates: " + o.error);
    }
  }
  // The entity buffer is dead the moment the feed has been consumed - nothing reads it after
  // finish() - so the borrow is handed back HERE, before the first buffered response needs it.
  // The retention block is NOT: the merge loop at the bottom reads stream.retained() directly, so
  // that one stays live for the whole call (it is a vector of non-trivially-destructible values
  // and cannot share the byte scratch - see PollBuffers).
  if (buffers != nullptr) stream.setEntityBuffer(nullptr);
  pollTrace(kPollTraceRtStream);  // the ~4,600 B retention block is what is live from here on
  const std::vector<StopTimeUpdate>& rt_updates = stream.retained();

  struct RouteVehicles {
    std::string route;
    bool ok = true;
  };
  std::vector<RouteVehicles> tv_ok_by_route;
  // ONE vehicle list for the whole cycle, parsed straight into (0.3.2-rc1). It used to be two -
  // a per-route vector the parser grew by doubling, and this accumulator which the insert() grew
  // by doubling again - so a 30-vehicle route asked for a 5,632 B contiguous block twice over,
  // every cycle, with the retention block live. With PollBuffers it is resident and neither
  // request happens at all.
  std::vector<TvVehicle> own_tv;
  std::vector<TvVehicle>& tv_all = buffers != nullptr ? buffers->tv : own_tv;
  tv_all.clear();
  // ...AND ONLY THE VEHICLES THIS CONFIG CAN USE (0.3.2-rc3). mergeStop() reaches a TvVehicle
  // through exactly one door - findTvByTrip(tv, u.trip_id), for a `u` drawn from `rt_updates` -
  // so a vehicle whose trip id is not in the retained updates is never read by anything. The
  // retained updates are already filtered to the configured routes and stops (setRouteFilter /
  // setStopFilter above), which makes "its trip is in rt_updates" exactly "it is relevant to a
  // configured stop", and it is the join key the merge itself uses rather than a proxy for it.
  //
  // The scan is linear over rt_updates (16 entries for the owner's two stops) per candidate
  // vehicle, on a string compare that fails on the first character almost always. That is cheap
  // against the nine std::strings it saves building and freeing.
  //
  // A failed or empty TripUpdates feed leaves rt_updates empty and this filter then keeps
  // nothing - which is correct rather than a degradation: with no realtime updates the merge
  // never calls findTvByTrip() at all, so an unfiltered vehicle list would be built, carried
  // through the cycle and read by no one.
  struct TripInRetained {
    static bool keep(const std::string& trip, const void* ctx) {
      const auto* updates = static_cast<const std::vector<StopTimeUpdate>*>(ctx);
      for (const auto& u : *updates) {
        if (u.trip_id == trip) return true;
      }
      return false;
    }
  };
  const TvFilter tv_filter(&TripInRetained::keep, &rt_updates);
  for (const auto& route : rt_routes) {
    FetchOutcome o = src.fetchTransitViewAppendEx(route, &tv_all, http, tv_filter);
    tv_ok_by_route.push_back({route, o.ok});
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
