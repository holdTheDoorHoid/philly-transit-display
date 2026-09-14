// Embedded list of Regional Rail station names accepted by SEPTA's Arrivals API `station=`
// parameter. See NOTES.md 6 for how this was derived and verified, and
// firmware/test/fixtures/rail_stations.json for the same list as JSON (for the web mock and
// web UI dev without hardware).
//
// Memory: kRailStationNames is a flat array of const char* literals. The strings themselves are
// literal data - the compiler places them in .rodata/flash on ESP32, not RAM. The pointer array
// costs kRailStationCount * sizeof(char*): 149 * 4 = 596 bytes on ESP32's 32-bit pointers,
// also flash-resident since the array itself is `const`.
#pragma once
#include <cstddef>

namespace transit {

// 149 station names verified live against Arrivals/index.php (NOTES.md 6). Six GTFS-static
// station names could not be matched to a working Arrivals name and are intentionally absent:
// "Airport Terminals C & D", "Delaware Valley University", "Fern Rock Transit Center",
// "Holmesburg Junction", "Norristown Transit Center", "Richard Allen Ln" - see NOTES.md 6 for
// what was tried.
extern const char* const kRailStationNames[];
extern const size_t kRailStationCount;

}  // namespace transit
