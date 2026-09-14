#include "demo_data.h"

using transit::Alert;
using transit::Arrival;
using transit::Epoch;
using transit::Snapshot;
using transit::Status;
using transit::StopSnapshot;

namespace transit_app {

namespace {

Arrival liveArrival(const char *trip, const char *vehicle, const char *destination, Epoch predicted, int16_t late_min, const char *seats) {
  Arrival a;
  a.trip = trip;
  a.vehicle = vehicle;
  a.destination = destination;
  a.predicted = predicted;
  a.scheduled = predicted - (Epoch)late_min * 60;
  a.late_min = late_min;
  a.late_known = true;
  a.status = Status::Live;
  a.stop_sequence = 35;
  a.seats = seats;
  return a;
}

Arrival scheduledArrival(const char *destination, Epoch scheduled) {
  Arrival a;
  a.destination = destination;
  a.predicted = 0;
  a.scheduled = scheduled;
  a.late_min = 0;
  a.late_known = false;
  a.status = Status::Scheduled;
  return a;
}

}  // namespace

Snapshot buildDemoSnapshot(Epoch now) {
  Snapshot snap;
  // Fixed 12s-old timestamp so the header always reads "updated 12 s ago",
  // matching DESIGN.md SS8's own example text - a deliberate demo choice,
  // not a bug.
  snap.generated = now - 12;
  snap.last_poll_ok = true;

  StopSnapshot southbound;
  southbound.key = "17-21332";
  southbound.fetched = snap.generated;
  southbound.ok = true;
  southbound.arrivals = {
    liveArrival("3667", "7477", "20th-Johnston", now + 4 * 60, 13, "FEW_SEATS_AVAILABLE"),
    liveArrival("3671", "7481", "20th-Johnston", now + 11 * 60, -1, "MANY_SEATS_AVAILABLE"),
    scheduledArrival("20th-Johnston", now + 19 * 60),
  };

  StopSnapshot northbound;
  northbound.key = "17-21297";
  northbound.fetched = snap.generated;
  northbound.ok = true;
  northbound.arrivals = {
    liveArrival("3652", "7412", "2nd-Market", now + 2 * 60, 0, "MANY_SEATS_AVAILABLE"),
    liveArrival("3658", "7439", "2nd-Market", now + 9 * 60, 6, "FEW_SEATS_AVAILABLE"),
    scheduledArrival("2nd-Market", now + 15 * 60),
  };

  snap.stops = {southbound, northbound};

  Alert alert;
  alert.route = "17";
  alert.text = "Detour on Route 17 near Grays Ferry Ave for construction through Friday.";
  alert.detours = {"Buses diverted via Passyunk Ave between Tasker and Wharton."};
  alert.current = true;
  snap.alerts = {alert};

  return snap;
}

}  // namespace transit_app
