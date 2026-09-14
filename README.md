# Philly Transit Display

A desk display for Philadelphia transit riders, running on the ESP32 "Cheap Yellow Display"
(CYD) family of boards. It shows the next buses, trolleys, or Regional Rail trains at the
stops you care about, with live lateness, and logs every arrival to the SD card so it can
tell you how the route actually performs over time.

Status: **pre-alpha, under active development.** Nothing is flashable yet.

- Design and decisions: [DESIGN.md](DESIGN.md)
- Data source research: [docs/research/septa-data-sources.md](docs/research/septa-data-sources.md)
- Hardware research: [docs/research/cyd-hardware.md](docs/research/cyd-hardware.md)

## What it will do

- Pick any SEPTA bus/trolley route and stop (or Regional Rail station) from a web page served by
  the device itself. No app, no cloud account.
- Show the next 2-3 arrivals per stop and direction with minutes-until and a late/early badge.
- Show SEPTA service alerts and detours for your routes.
- Log predictions and inferred arrivals to the SD card; view lateness by hour and weekday,
  headway bunching, ghost buses, and prediction accuracy in the web UI or on the screen.
- Work on the 2.4", 2.8", and 3.5" CYD variants from one codebase.
- Be portable: the real-time decoder speaks standard GTFS-Realtime, so other agencies can be
  added without touching the display or stats code.

## License

MIT. See [LICENSE](LICENSE).
