// Where in a poll cycle the byte-addressable heap goes (diag branch, 2026-09-17).
//
// THE QUESTION THIS EXISTS TO ANSWER. On the owner's board the largest MALLOC_CAP_8BIT block falls
// from 23.5 KB at boot to under 12 KB after three or four minutes, the next fetch throws bad_alloc
// (net_poller.cpp's catch), and from then on every 30 s cycle fails AND loses another 2-4 KB of
// free8 until the heap-wedge self-heal reboots. Two questions: which STAGE drives the largest block
// down, and which stage LEAKS on the failure path. Both are answered by the same thing - free8 and
// largest sampled at every stage boundary of pollOnce(), including inside the bad_alloc catches.
//
// WHY A RING AND NOT SERIAL. tracePoll() already printed exactly these two numbers per stage
// (net_poller.cpp, DESIGN.md SS2.1), but only under -DTRANSIT_HEAP_TRACE and only to Serial, and on
// this bench opening /dev/ttyUSB0 resets the board - so the measurement cannot be taken that way at
// all. The samples therefore go into a fixed .bss ring that GET /api/debug/ui reads out, that being
// the one endpoint deliberately left outside refuseIfLowHeap()'s 503 gate (web_server.cpp) and so
// the only one that still answers when the heap is gone. The Serial line is kept, still behind the
// flag, so the -https envs' numbers stay comparable with the ones DESIGN.md SS2.1 records.
//
// WHAT IT COSTS, because an instrument that moves the thing it measures is worthless: 64 entries x
// 12 B = 768 B of .bss and nothing on the heap. A mark is two heap_caps_ calls and a 12-byte store
// under a portMUX; no String, no std::string, no allocation, and the stage names are const char*
// literals in flash. The readout side's cost is in web_server.cpp, where it is bounded to one
// ~1 KB static buffer rather than a few hundred ArduinoJson slots - see kHeapTraceJsonBytes.
#pragma once
#include <cstddef>
#include <cstdint>

namespace transit_app {

// Stage ids. The VALUES are part of the wire format GET /api/debug/ui reports and the reader script
// decodes, so append new ones at the end and never renumber. kHeapTraceStages below is the parallel
// name table and must stay in step (a static_assert in heap_trace.cpp checks the count).
enum HeapTraceStage : uint8_t {
  kStagePollStart = 0,     // pollOnce() entry, before the Config copy
  kStagePreTransit = 1,    // after the Config copy and the bus/rail split, before any fetch
  kStageRtStream = 2,      // transit_core: the GTFS-RT TripUpdates stream has been fetched+filtered
  kStageTvRoute = 3,       // transit_core: one route's TransitView fetch+parse finished
  kStageSchedStop = 4,     // transit_core: one stop's BusSchedules lookup finished (hit or fetch)
  kStageMerge = 5,         // transit_core: every stop merged into the Snapshot
  kStageOomTransit = 6,    // INSIDE net_poller's bad_alloc catch around the arrival fetch
  kStagePostTransit = 7,   // after the arrivals Snapshot was published
  kStagePreAlerts = 8,
  kStagePostAlerts = 9,    // after collectAlerts() and any second publish
  kStagePreWeather = 10,
  kStagePostWeather = 11,
  kStagePreBikes = 12,
  kStagePostBikes = 13,    // after the 400 KB Indego stream
  kStagePreLiveness = 14,
  kStagePostLiveness = 15,
  kStagePostTracker = 16,  // after the ArrivalTracker observe loop
  kStagePostSd = 17,       // after the SD append loop and the Indego samples
  kStageCycleEnd = 18,     // pollOnce() return
  kStageOomCycle = 19,     // INSIDE pollerTask's bad_alloc catch (the optional tail threw)
  kStageOomBikes = 20,     // INSIDE bike_service getBikes()'s catch
  kStageOomProxy = 21,     // INSIDE proxy_worker runJob()'s catch
  kStageOomStatus = 22,    // INSIDE net_poller tryGetPollStatus()'s catch
  kHeapTraceStageCount = 23,
};

// Flash-resident literals, indexed by the ids above. Never copied into RAM.
extern const char *const kHeapTraceStages[kHeapTraceStageCount];

// One sample. 12 bytes, POD, no padding surprises on the 32-bit ABI.
struct HeapTraceEntry {
  uint16_t cycle;    // pollOnce() cycles since boot, wrapping at 65535 (~22 days at 30 s)
  uint8_t stage;     // HeapTraceStage
  uint8_t flags;     // reserved, always 0
  uint32_t free8;    // heap_caps_get_free_size(MALLOC_CAP_8BIT)
  uint32_t largest;  // heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
};

// ~3 cycles of history on the healthy path, more on the failure path (a failed cycle skips stages).
// The readout is incremental (`since`), so a sampler polling faster than this fills never loses a
// row; the size is only the depth a single cold GET can recover.
constexpr size_t kHeapTraceCap = 64;

// Records free8 + largest for `stage` against the current cycle number. Safe from any task (the
// poller, the AsyncTCP task and the display task all reach it through the catch sites), not from an
// ISR. Allocates nothing.
void heapTraceMark(uint8_t stage);

// Starts a new cycle: bumps the cycle number, then marks kStagePollStart. Called once per
// pollOnce().
void heapTraceBeginCycle();

// Total marks recorded since boot. Also the sequence number the NEXT mark will get, so a reader
// passes the value it last saw back as `since` and gets only what it has not seen.
uint32_t heapTraceSeq();

// Copies out at most `max` entries whose sequence number is >= `since`, oldest first, into `out`.
// Returns how many were written and, in *out_first_seq, the sequence number of out[0] - which may
// be GREATER than `since` if the ring wrapped past it, and that gap is itself worth reporting.
size_t heapTraceRead(uint32_t since, HeapTraceEntry *out, size_t max, uint32_t *out_first_seq);

}  // namespace transit_app
