#include "transit_stats/aggregate.h"

#include <cmath>
#include <cstring>

namespace transit_stats {

namespace {

// ---- deterministic America/New_York local time, no OS tz database -------------------------
//
// Howard Hinnant's civil-calendar algorithms (public domain):
// http://howardhinnant.github.io/date_algorithms.html
// days_from_civil / civil_from_days convert between a proleptic-Gregorian (y, m, d) and the
// number of days since 1970-01-01 (which is day 0), correct for all dates this project cares
// about (2020s-2030s).

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) q--;
  return q;
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;    // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);                    // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;       // [0, 399]
  const int64_t yr = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                     // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                          // [0, 11]
  d = doy - (153 * mp + 2) / 5 + 1;                                                 // [1, 31]
  m = mp + (mp < 10 ? 3 : -9);                                                      // [1, 12]
  y = yr + (m <= 2);
}

// 0 = Sunday .. 6 = Saturday (struct tm's tm_wday convention). Day 0 (1970-01-01) was a Thursday.
int weekdayFromDays(int64_t days) { return static_cast<int>(((days % 7) + 7 + 4) % 7); }

// Day-of-month of the n-th weekday==Sunday occurrence in (year, month).
int64_t nthSundayOfMonth(int64_t year, unsigned month, int n) {
  int64_t day1 = daysFromCivil(year, month, 1);
  int w = weekdayFromDays(day1);
  int firstSundayDom = 1 + ((7 - w) % 7);
  int sundayDom = firstSundayDom + (n - 1) * 7;
  return daysFromCivil(year, month, static_cast<unsigned>(sundayDom));
}

int horizonBucketIndex(int32_t horizon_s) {
  for (int i = 0; i < 4; i++) {
    if (horizon_s <= kHorizonBuckets[i]) return i;
  }
  return -1;
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

int percentileFromHist(const LatenessBucket& b, double pct) {
  if (b.n == 0) return 0;
  uint32_t target = static_cast<uint32_t>(std::ceil(static_cast<double>(b.n) * pct));
  if (target == 0) target = 1;
  uint32_t cum = 0;
  for (int i = 0; i < 71; i++) {
    cum += b.hist[i];
    if (cum >= target) return i - 10;
  }
  return 60;
}

void copyTripTruncated(const std::string& s, char (&dst)[16]) {
  size_t n = s.size();
  if (n > 15) n = 15;
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

bool tripMatches(const char (&buf)[16], const std::string& s) {
  char key[16];
  copyTripTruncated(s, key);
  return std::strncmp(buf, key, 16) == 0;
}

}  // namespace

void americaNewYorkLocalHourWeekday(transit::Epoch utc, int& hour_local, int& weekday_local) {
  const int64_t days = floorDiv(utc, 86400);
  int64_t y;
  unsigned mo, da;
  civilFromDays(days, y, mo, da);
  (void)mo;
  (void)da;

  const int64_t marchSecondSunday = nthSundayOfMonth(y, 3, 2);
  const int64_t novFirstSunday = nthSundayOfMonth(y, 11, 1);
  const transit::Epoch dstStartUtc = marchSecondSunday * 86400 + 7 * 3600;  // 02:00 EST = 07:00 UTC
  const transit::Epoch dstEndUtc = novFirstSunday * 86400 + 6 * 3600;       // 02:00 EDT = 06:00 UTC

  const bool isDst = (utc >= dstStartUtc) && (utc < dstEndUtc);
  const int64_t offsetSeconds = isDst ? -4 * 3600 : -5 * 3600;

  const int64_t localEpoch = utc + offsetSeconds;
  const int64_t localDays = floorDiv(localEpoch, 86400);
  const int64_t localSecOfDay = localEpoch - localDays * 86400;

  hour_local = static_cast<int>(localSecOfDay / 3600);
  weekday_local = weekdayFromDays(localDays);
}

StatsAggregator::StatsAggregator(std::string stop_key, transit::Epoch window_start,
                                  transit::Epoch window_end, TzHourWeekdayFn tz_fn)
    : stop_key_(std::move(stop_key)),
      window_start_(window_start),
      window_end_(window_end),
      tz_fn_(tz_fn) {}

void StatsAggregator::feedLine(const char* line, size_t len) {
  LogEvent ev;
  if (!fromCsv(line, len, ev)) return;
  if (ev.stop_key != stop_key_) return;
  if (ev.ts < window_start_ || ev.ts >= window_end_) return;

  switch (ev.event) {
    case EventType::Pred: handlePred(ev); break;
    case EventType::Arrive: handleArrive(ev); break;
    case EventType::Ghost: handleGhost(ev); break;
    case EventType::NoShow: handleNoShow(ev); break;
    case EventType::Outage: handleOutage(ev); break;
    case EventType::Bike: break;  // per-stop only; OverviewAggregator handles Bike rows
  }
}

PendingPred* StatsAggregator::findPending(const std::string& trip) {
  for (auto& p : pending_) {
    if (p.in_use && tripMatches(p.trip, trip)) return &p;
  }
  return nullptr;
}

PendingPred& StatsAggregator::allocPending(const std::string& trip) {
  for (auto& p : pending_) {
    if (!p.in_use) {
      p = PendingPred{};
      p.in_use = true;
      copyTripTruncated(trip, p.trip);
      p.seq = ++pending_seq_;
      return p;
    }
  }
  PendingPred* victim = &pending_[0];
  for (auto& p : pending_) {
    if (p.seq < victim->seq) victim = &p;
  }
  *victim = PendingPred{};
  victim->in_use = true;
  copyTripTruncated(trip, victim->trip);
  victim->seq = ++pending_seq_;
  return *victim;
}

void StatsAggregator::handlePred(const LogEvent& ev) {
  if (!ev.horizon_s.has_value() || !ev.predicted_ts.has_value()) return;
  const int idx = horizonBucketIndex(*ev.horizon_s);
  if (idx < 0) return;  // e.g. the first-sighting row, far from any of the 4 tracked buckets

  PendingPred* p = findPending(ev.trip);
  if (!p) p = &allocPending(ev.trip);
  p->seq = ++pending_seq_;
  p->predicted_offset_s[idx] = static_cast<int32_t>(*ev.predicted_ts - window_start_);
  p->bucket_has |= static_cast<uint8_t>(1u << idx);
}

void StatsAggregator::handleArrive(const LogEvent& ev) {
  samples_++;

  if (!ev.seats.empty()) {
    const int level = seatsLevel(ev.seats);
    if (level >= 0) {
      // Same "actual_ts if known, else ts" bucketing convention as the lateness buckets just
      // below -- this is a per-arrival-event time, so it follows the same rule.
      const transit::Epoch bucket_time = ev.actual_ts.value_or(ev.ts);
      int hour = 0, wd = 0;
      tz_fn_(bucket_time, hour, wd);

      CrowdingBucket& hb = crowd_by_hour_[hour];
      hb.n++;
      hb.sum_level += static_cast<uint32_t>(level);
      hb.dist[level]++;

      CrowdingBucket& wb = crowd_by_weekday_[wd];
      wb.n++;
      wb.sum_level += static_cast<uint32_t>(level);
      wb.dist[level]++;
    }
  }

  if (ev.headway_s.has_value() && *ev.headway_s > 0) {
    int hour = 0, wd = 0;
    tz_fn_(ev.ts, hour, wd);  // wait_by_hour buckets by the row's own ts, per DESIGN §9.3
    WaitBucket& wb = wait_by_hour_[hour];
    wb.n++;
    const uint32_t gap = static_cast<uint32_t>(*ev.headway_s);
    wb.sum_gap_s += gap;
    if (gap > wb.max_gap_s) wb.max_gap_s = gap;
  }

  const bool late_known = ev.late_min.has_value();
  if (late_known) {
    late_known_count_++;
    sum_late_known_ += *ev.late_min;
    if (*ev.late_min >= 0 && *ev.late_min <= 5) on_time_count_++;  // SEPTA on-time (see header)

    int32_t clamped = *ev.late_min;
    if (clamped < -10) clamped = -10;
    if (clamped > 60) clamped = 60;
    const int bin = clamped + 10;

    const transit::Epoch bucket_time = ev.actual_ts.value_or(ev.ts);
    int hour = 0, wd = 0;
    tz_fn_(bucket_time, hour, wd);

    LatenessBucket& hb = by_hour_[hour];
    hb.n++;
    hb.sum_late_min += *ev.late_min;
    hb.hist[bin]++;

    LatenessBucket& wb = by_weekday_[wd];
    wb.n++;
    wb.sum_late_min += *ev.late_min;
    wb.hist[bin]++;
  }

  if (ev.headway_s.has_value()) {
    headway_.n++;
    if (has_prev_scheduled_ && ev.scheduled_ts.has_value()) {
      const int64_t sched_gap = *ev.scheduled_ts - prev_scheduled_ts_;
      if (sched_gap > 0) {
        const double ratio = static_cast<double>(*ev.headway_s) / static_cast<double>(sched_gap);
        if (ratio < 0.40) headway_.bunched++;
        if (ratio > 1.75) headway_.gapped++;
        int bin = static_cast<int>(ratio / 0.1);
        if (bin < 0) bin = 0;
        if (bin > 19) bin = 19;
        headway_.ratio_hist[bin]++;
      }
    }
  }
  if (ev.scheduled_ts.has_value()) {
    prev_scheduled_ts_ = *ev.scheduled_ts;
    has_prev_scheduled_ = true;
  }

  if (PendingPred* p = findPending(ev.trip)) {
    const transit::Epoch actual_ts = ev.actual_ts.value_or(ev.ts);
    for (int i = 0; i < 4; i++) {
      if (!(p->bucket_has & (1u << i))) continue;
      const transit::Epoch predicted_ts = window_start_ + p->predicted_offset_s[i];
      const int32_t err = static_cast<int32_t>(predicted_ts - actual_ts);
      PredictionBucket& pb = prediction_[i];
      pb.n++;
      pb.sum_abs_err_s += (err < 0 ? -err : err);
      pb.sum_err_s += err;
    }
    p->in_use = false;  // consumed: forget it, matching a pred row to at most one arrive
  }
}

void StatsAggregator::handleGhost(const LogEvent& ev) {
  ghost_++;
  int hour = 0, wd = 0;
  tz_fn_(ev.ts, hour, wd);
  wait_by_hour_[hour].ghost++;
}

void StatsAggregator::handleNoShow(const LogEvent& ev) {
  noshow_++;
  int hour = 0, wd = 0;
  tz_fn_(ev.ts, hour, wd);
  wait_by_hour_[hour].noshow++;
}

void StatsAggregator::handleOutage(const LogEvent& ev) {
  if (ev.note == "end") {
    if (outage_pending_) {
      outage_seconds_ += static_cast<int64_t>(ev.ts - outage_pending_start_);
      outage_pending_ = false;
    }
  } else if (ev.note.empty()) {
    outage_pending_ = true;
    outage_pending_start_ = ev.ts;
  }
  // note == "no_live_vehicles" (the noshow-vs-outage fallback from ArrivalTracker) is a single
  // informational row, not one end of a start/end pair; it deliberately does not affect
  // outage_min. See tracker.cpp's reapVanishedAndExpired() for why it is tagged that way.
}

double StatsAggregator::onTimePct() const {
  if (samples_ == 0) return 0.0;
  return 100.0 * static_cast<double>(on_time_count_) / static_cast<double>(samples_);
}

double StatsAggregator::meanLateMin() const {
  if (late_known_count_ == 0) return 0.0;
  return static_cast<double>(sum_late_known_) / static_cast<double>(late_known_count_);
}

int StatsAggregator::findWorstHour() const {
  int best_hour = -1;
  double best_mean = -1e18;
  for (int h = 0; h < 24; h++) {
    if (by_hour_[h].n == 0) continue;
    const double mean = static_cast<double>(by_hour_[h].sum_late_min) / by_hour_[h].n;
    if (mean > best_mean) {
      best_mean = mean;
      best_hour = h;
    }
  }
  return best_hour;
}

void StatsAggregator::toJson(ArduinoJson::JsonDocument& doc) const {
  doc["stop"] = stop_key_;
  doc["days"] = static_cast<int32_t>((window_end_ - window_start_) / 86400);
  doc["samples"] = samples_;
  doc["on_time_pct"] = round1(onTimePct());
  doc["mean_late_min"] = round1(meanLateMin());

  ArduinoJson::JsonArray by_hour = doc["by_hour"].to<ArduinoJson::JsonArray>();
  for (int h = 0; h < 24; h++) {
    const LatenessBucket& b = by_hour_[h];
    ArduinoJson::JsonObject o = by_hour.add<ArduinoJson::JsonObject>();
    o["h"] = h;
    o["n"] = b.n;
    o["mean"] = b.n ? round1(static_cast<double>(b.sum_late_min) / b.n) : 0.0;
    o["p50"] = percentileFromHist(b, 0.50);
    o["p90"] = percentileFromHist(b, 0.90);
  }

  ArduinoJson::JsonArray by_weekday = doc["by_weekday"].to<ArduinoJson::JsonArray>();
  for (int wd = 0; wd < 7; wd++) {
    const LatenessBucket& b = by_weekday_[wd];
    ArduinoJson::JsonObject o = by_weekday.add<ArduinoJson::JsonObject>();
    o["wd"] = wd;  // 0 = Sunday .. 6 = Saturday
    o["n"] = b.n;
    o["mean"] = b.n ? round1(static_cast<double>(b.sum_late_min) / b.n) : 0.0;
    o["p50"] = percentileFromHist(b, 0.50);
    o["p90"] = percentileFromHist(b, 0.90);
  }

  ArduinoJson::JsonObject headway = doc["headway"].to<ArduinoJson::JsonObject>();
  headway["n"] = headway_.n;
  headway["bunched"] = headway_.bunched;
  headway["gapped"] = headway_.gapped;
  ArduinoJson::JsonArray ratio_hist = headway["ratio_hist"].to<ArduinoJson::JsonArray>();
  for (uint16_t v : headway_.ratio_hist) ratio_hist.add(v);

  doc["ghost"] = ghost_;
  doc["noshow"] = noshow_;
  doc["outage_min"] = static_cast<int32_t>(outage_seconds_ / 60);

  ArduinoJson::JsonArray prediction = doc["prediction"].to<ArduinoJson::JsonArray>();
  for (int i = 0; i < 4; i++) {
    const PredictionBucket& pb = prediction_[i];
    ArduinoJson::JsonObject o = prediction.add<ArduinoJson::JsonObject>();
    o["horizon_s"] = kHorizonBuckets[i];
    o["n"] = pb.n;
    o["mae_s"] = pb.n ? static_cast<int32_t>(std::lround(static_cast<double>(pb.sum_abs_err_s) / pb.n)) : 0;
    o["bias_s"] = pb.n ? static_cast<int32_t>(std::lround(static_cast<double>(pb.sum_err_s) / pb.n)) : 0;
  }

  ArduinoJson::JsonObject crowding = doc["crowding"].to<ArduinoJson::JsonObject>();
  ArduinoJson::JsonArray crowd_by_hour = crowding["by_hour"].to<ArduinoJson::JsonArray>();
  for (int h = 0; h < 24; h++) {
    const CrowdingBucket& b = crowd_by_hour_[h];
    ArduinoJson::JsonObject o = crowd_by_hour.add<ArduinoJson::JsonObject>();
    o["h"] = h;
    o["n"] = b.n;
    o["mean"] = b.n ? round1(static_cast<double>(b.sum_level) / b.n) : 0.0;
    ArduinoJson::JsonArray dist = o["dist"].to<ArduinoJson::JsonArray>();
    for (uint16_t v : b.dist) dist.add(v);
  }
  ArduinoJson::JsonArray crowd_by_weekday = crowding["by_weekday"].to<ArduinoJson::JsonArray>();
  for (int wd = 0; wd < 7; wd++) {
    const CrowdingBucket& b = crowd_by_weekday_[wd];
    ArduinoJson::JsonObject o = crowd_by_weekday.add<ArduinoJson::JsonObject>();
    o["wd"] = wd;
    o["n"] = b.n;
    o["mean"] = b.n ? round1(static_cast<double>(b.sum_level) / b.n) : 0.0;
    ArduinoJson::JsonArray dist = o["dist"].to<ArduinoJson::JsonArray>();
    for (uint16_t v : b.dist) dist.add(v);
  }

  ArduinoJson::JsonArray wait_by_hour = doc["wait_by_hour"].to<ArduinoJson::JsonArray>();
  for (int h = 0; h < 24; h++) {
    const WaitBucket& b = wait_by_hour_[h];
    ArduinoJson::JsonObject o = wait_by_hour.add<ArduinoJson::JsonObject>();
    o["h"] = h;
    o["n"] = b.n;
    o["mean_gap_s"] = b.n ? static_cast<int32_t>(std::lround(static_cast<double>(b.sum_gap_s) / b.n)) : 0;
    o["max_gap_s"] = static_cast<int32_t>(b.max_gap_s);
    o["ghost"] = b.ghost;
    o["noshow"] = b.noshow;
  }
}

}  // namespace transit_stats
