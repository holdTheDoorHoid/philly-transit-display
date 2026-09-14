// Indego bike share status (DESIGN.md 4.9): a streaming reader for Bicycle Transit's status
// feed at http://bts-status.bicycletransit.workers.dev/phl. Arduino-independent: compiles on
// the host (native tests) and on ESP32.
//
// The feed is a ~400 KB GeoJSON FeatureCollection, one feature per station, each carrying a
// "bikes" array of per-dock entries that can push a single feature to 2-3 KB. The device never
// buffers the body: StatusStream scans it feature by feature, tracking brace depth *inside*
// the top-level "features" array (with correct JSON string handling, so braces and quotes
// inside a station name don't confuse boundary detection), and holds at most one feature at a
// time, capped at kMaxFeatureBytes. A feature that exceeds the cap is dropped (counted in
// featuresSkipped()) without disturbing the parse of the next one.
//
// Memory footprint: sizeof(StatusStream) itself is small (a handful of vector/bool/size_t
// members, well under 100 bytes); the one-feature scratch buffer is reserved once at
// kMaxFeatureBytes (6144 bytes) and never grows past that regardless of how large a single
// feature actually is, so total footprint stays flat and independent of the ~400 KB body size.
// stations() grows only with the number of filter ids that actually matched (a handful of
// short-lived Station entries with a small std::string name each).
//
// Before spending an ArduinoJson parse, a cheap text scan checks `"id":<n>` (tolerating
// whitespace after the colon) against the configured filter; only matching features are handed
// to ArduinoJson, and with a filter document limited to the properties Station actually needs
// (skipping "geometry" and the per-dock "bikes" array entirely) so the parse itself stays
// small too.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace indego {

struct Station {
  int id = 0;
  std::string name;
  int bikes = -1;         // bikesAvailable
  int ebikes = -1;        // electricBikesAvailable
  int classic = -1;       // classicBikesAvailable
  int docks = -1;         // docksAvailable
  int total_docks = -1;   // totalDocks
  bool active = false;    // kioskPublicStatus == "Active"
};

// Streaming scanner for one status feed body. Construct once per poll, set the station filter,
// then feed HTTP body bytes to push() as they arrive (any chunk size, including 1 byte at a
// time - this is exercised by tests) and call finish() when the body is complete or the
// transfer is aborted. Not thread-safe; not reentrant.
class StatusStream {
 public:
  // Largest single feature (raw JSON text, brace to brace) this scanner will buffer and parse.
  // The real feed's largest observed feature is ~3 KB; a feature bigger than this cap is
  // skipped (see featuresSkipped()) rather than grown into, so one oversized feature never
  // costs the scanner unbounded memory.
  static constexpr size_t kMaxFeatureBytes = 6144;

  // Reserves the one-feature scratch buffer up front so it never reallocates mid-stream.
  StatusStream() { feature_buf_.reserve(kMaxFeatureBytes); }

  // Restricts reported stations to these ids. An empty filter (the default) reports nothing.
  // Call before push(); not safe to change mid-stream.
  void setStationFilter(const std::vector<int>& ids);

  // Feeds the next chunk of the HTTP response body. May be called any number of times with any
  // chunk sizes; parse state persists across calls, and results are identical no matter how
  // the body is split. Returns false once every id in the configured filter has been found (the
  // caller may then abort the transfer - the rest of the ~400 KB body is not needed) or after
  // finish() has been called; true otherwise. Malformed or truncated input is tolerated: it
  // never crashes or reads out of bounds, it just yields fewer stations.
  bool push(const uint8_t* data, size_t len);

  // Signals end of input. Any feature left mid-capture (a truncated body) is discarded without
  // being reported. Idempotent; safe to call even if push() was never called.
  void finish();

  // Matched stations, in feed order.
  const std::vector<Station>& stations() const { return stations_; }

  // Total top-level features encountered (parsed or not, matched or not).
  size_t featuresSeen() const { return features_seen_; }

  // Subset of featuresSeen() that were dropped for exceeding kMaxFeatureBytes.
  size_t featuresSkipped() const { return features_skipped_; }

 private:
  enum class State : uint8_t {
    kSeekKey,          // scanning for the literal "features" key
    kSeekColon,        // found the key, scanning for ':'
    kSeekBracket,      // found the colon, scanning for the array's opening '['
    kBetweenFeatures,  // inside the array, between features (or before the first one)
    kInFeature,        // buffering one feature's bytes, tracking string-aware brace depth
    kIgnoreRest,       // the array has closed; remaining bytes are not interesting
  };

  void processByte(uint8_t b);
  void onFeatureClosed();
  void appendFeatureByte(uint8_t b);

  State state_ = State::kSeekKey;
  size_t key_match_pos_ = 0;

  // kInFeature parse state.
  int depth_ = 0;
  bool in_string_ = false;
  bool escape_ = false;
  bool feature_oversized_ = false;
  std::vector<uint8_t> feature_buf_;

  std::vector<int> filter_ids_;     // as configured, for reference
  std::vector<int> remaining_ids_;  // filter ids not yet matched; empties out as features match

  std::vector<Station> stations_;
  size_t features_seen_ = 0;
  size_t features_skipped_ = 0;
  bool done_ = false;
};

// "http://bts-status.bicycletransit.workers.dev/phl"
std::string statusUrl();

}  // namespace indego
