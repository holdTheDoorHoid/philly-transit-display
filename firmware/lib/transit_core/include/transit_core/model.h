// Shared data model for Philly Transit Display.
// Arduino-independent: compiles on the host (native tests) and on ESP32.
// This header is authoritative; see DESIGN.md §5-§9. Change it deliberately.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace transit {

// Unix seconds, UTC.
using Epoch = int64_t;

enum class Mode : uint8_t { Bus, Trolley, Subway, Rail };

// Where an arrival row's time came from.
enum class Status : uint8_t {
  Live,       // realtime prediction exists (GTFS-RT or Arrivals API)
  Scheduled,  // schedule only, no live vehicle yet
  Skipped,    // realtime says this stop is skipped (detour)
  Unknown
};

// How trustworthy one stop's rows are right now (DESIGN.md 7). This is about the DATA, while
// StopSnapshot::ok is about the FETCH - see StopSnapshot for how the two relate.
enum class Health : uint8_t {
  Live,          // at least one row came from a fresh realtime prediction
  ScheduleOnly,  // rows are scheduled times only: subway (no realtime source exists at all), or
                 // a stop whose realtime source returned/failed with nothing usable
  Stale,         // the realtime feed's own timestamp is too old, or implausibly in the future
                 // (DESIGN.md 4.7): live rows were demoted to Scheduled or dropped
  Unavailable    // a source this stop needs failed and there is nothing trustworthy to show
};

// One configured stop/direction the user wants displayed. Mirrors config.json "stops[]".
struct StopConfig {
  std::string key;        // unique, stable; join key in the log, e.g. "17-21332"
  Mode mode = Mode::Bus;
  std::string route;      // "17", "T4", "G1"; rail: line code or "" for all lines
  std::string stop_id;    // SEPTA stop_id for bus/trolley/subway; "" for rail
  std::string station;    // Regional Rail station name for Arrivals API; "" otherwise
  std::string direction;  // bus: GTFS direction_id "0"/"1"; rail: "N"/"S"/"" (both)
  std::string headsign;   // e.g. "20th-Johnston"; shown in the panel title
  std::string label;      // user label, e.g. "17 Southbound"
  std::string stop_name;  // e.g. "19th St & Mifflin St"
  uint8_t show = 3;       // arrival rows to display, 1..4
  double lat = 0;         // stop coordinates from SEPTA's Stops API, for weather (DESIGN.md 4.8);
  double lng = 0;         // 0/0 = unknown (older configs, Regional Rail)
  std::string title_style = "label_dest";  // DESIGN.md 6: label_dest | label | route_dest_stop | custom
  std::string title_text;                  // used when title_style == "custom"
  std::string alt_of;                      // key of the stop this one is an alternative to, "" = always shown
  uint8_t alt_after_min = 15;              // show while alt_of's next arrival is >= this many minutes out
};

// One upcoming vehicle at one configured stop.
struct Arrival {
  std::string trip;         // realtime trip id (SEPTA: identical in GTFS-RT and TransitView)
  std::string vehicle;      // vehicle id, may be empty
  std::string destination;  // headsign for this vehicle, may be empty
  Epoch predicted = 0;      // predicted arrival at this stop, 0 if none
  Epoch scheduled = 0;      // matched scheduled time at this stop, 0 if unknown
  int16_t late_min = 0;     // agency-reported lateness (TransitView `late`); negative = early
  bool late_known = false;  // late_min is meaningful
  Status status = Status::Unknown;
  uint16_t stop_sequence = 0;  // GTFS-RT stop_sequence of this stop in this trip, 0 if unknown
  std::string seats;        // SEPTA estimated_seat_availability, may be empty
  std::string sched_trip;   // static-GTFS trip id (SchedEntry::trip_id) this row was matched to,
                             // "" if no schedule entry matched. Set both when a live arrival was
                             // joined to a schedule entry and for schedule-only rows (where it
                             // equals the row's own `trip`), so the stats tracker can reconcile
                             // scheduled-vs-live records for the same static trip later.

  // Time to display: predicted if live, else scheduled.
  Epoch effective() const { return predicted ? predicted : scheduled; }
};

// The current view of one configured stop after merging all sources.
//
// `ok` and `health` answer two different questions and callers should read both:
//   ok      "did every source this stop needs answer with usable, current data?" - false when a
//           fetch/parse failed for a required source, and also false for Stale/Unavailable, so
//           Snapshot::last_poll_ok ("every stop's required sources succeeded") is just the AND
//           of these. mergeStop()/mergeRail() set it from the SourceStatus their caller passes.
//   health  what the rows that ARE here are worth (see Health). A stop can be ok=false with rows
//           still on screen (e.g. a truncated live feed but a good schedule -> ScheduleOnly).
struct StopSnapshot {
  std::string key;        // StopConfig::key
  Epoch fetched = 0;      // when this snapshot was assembled (local clock)
  Epoch source_ts = 0;    // when the AGENCY produced the data: GTFS-RT FeedHeader.timestamp for
                           // bus/trolley, the newest TransitView vehicle timestamp when that is
                           // all there is, `fetched` for schedule-only/rail. 0 = unknown age (a
                           // feed that published no timestamp); never invented - see merge.h.
  Health health = Health::Live;
  bool ok = true;         // false if the sources for this stop failed, or the data is stale
  std::string error;      // short human-readable reason when !ok, e.g. "SEPTA schedule
                           // unavailable", "live feed truncated", "live feed stale"
  std::vector<Arrival> arrivals;  // sorted by effective() ascending; may exceed StopConfig::show
};

// A service alert or detour for one route.
struct Alert {
  std::string route;                 // "17"
  std::string text;                  // plain text, HTML stripped, first ~240 chars
  std::vector<std::string> detours;  // plain-text detour summaries
  bool current = true;
};

// Everything the UI and web server need, swapped atomically by the poller.
struct Snapshot {
  Epoch generated = 0;
  bool last_poll_ok = true;
  std::string last_error;
  std::vector<StopSnapshot> stops;
  std::vector<Alert> alerts;
};

}  // namespace transit
