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

  // Time to display: predicted if live, else scheduled.
  Epoch effective() const { return predicted ? predicted : scheduled; }
};

// The current view of one configured stop after merging all sources.
struct StopSnapshot {
  std::string key;        // StopConfig::key
  Epoch fetched = 0;      // when this snapshot was assembled
  bool ok = true;         // false if the sources for this stop failed
  std::string error;      // short human-readable reason when !ok
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
