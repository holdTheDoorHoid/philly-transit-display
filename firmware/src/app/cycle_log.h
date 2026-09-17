// One row per poll cycle, two hours deep, so the thing that FRAGMENTS the heap can finally be
// caught (0.3.2-rc1).
//
// THE QUESTION THIS EXISTS TO ANSWER, and why heap_trace.h cannot. The per-stage ring holds 64
// entries, which on a healthy path is three cycles: it says precisely where inside a cycle the
// heap goes, and it is what identified the retention block as the allocation that failed. What it
// cannot say is what happened BEFORE. On the owner's board on 2026-09-17 the resting figures were
// free8 39-40.7 KB with a largest block never under 22.5 KB for thirty-five minutes, and by the
// time anyone looked they were free8 17-20 KB with a largest block of 3,444 B. The cycles in which
// that happened had long since scrolled out of a 64-entry ring, and the trigger is still unknown.
//
// So: one 16-byte row per cycle, 240 of them - two hours at a 30 s cadence, considerably more once
// the failure backoff stretches the interval. Each row carries the poll-start free8 and largest
// block (the two resting numbers whose decay is the symptom), the LOWEST free8 seen anywhere in
// that cycle, and a flag word saying which optional work ran. A schedule refetch, an alerts fetch,
// a weather fetch and the 400 KB Indego stream are the four things that do not happen every cycle,
// so if the collapse correlates with one of them, this is the record that shows it.
//
// WHAT IT COSTS: 240 x 16 = 3,840 B of .bss and nothing on the heap, plus ~600 B of render buffer
// shared with the trace ring's (web_server.cpp). Recording a row is a 16-byte store under the same
// kind of portMUX the trace ring uses; noting a free8 sample is a compare and a store. No String,
// no std::string, no allocation, on any path.
//
// WHAT THE "min free8" FIGURE IS, stated honestly: the lowest of the samples this firmware
// actually takes during the cycle - every heap_trace stage boundary, plus every accept-time
// admission check on the AsyncTCP task. It is NOT a true minimum; a trough that opens and closes
// between two samples is invisible to it, which is exactly what happened to rc1's 696 B reading.
// heap_caps_get_minimum_free_size() is the true minimum and is reported separately as `min_free8`,
// but it is since BOOT and cannot be attributed to a cycle. Read the two together: this column
// says which cycle was tight, that one says how tight the board has ever been.
#pragma once
#include <cstddef>
#include <cstdint>

namespace transit_app {

// What ran in a cycle. The VALUES are part of the wire format GET /api/debug/ui?log=1 reports, so
// append new ones and never renumber.
enum CycleFlag : uint16_t {
  kCycleSchedRefetch = 1u << 0,  // at least one stop's BusSchedules was actually fetched
  kCycleAlertsFetch = 1u << 1,   // an alert feed answered (and a second Snapshot was published)
  kCycleWeather = 1u << 2,       // the Open-Meteo forecast was refreshed
  kCycleBikes = 1u << 3,         // the ~400 KB Indego status feed was streamed
  kCycleOom = 1u << 4,           // a std::bad_alloc was caught somewhere in this cycle
  kCycleFailed = 1u << 5,        // the poll reported failure (last_poll_ok false)
  kCycleUnsynced = 1u << 6,      // the clock was not sane, so schedules were not cached
};

// One cycle. 16 bytes, POD, no padding surprises on the 32-bit ABI.
struct CycleLogEntry {
  uint32_t uptime_s;      // seconds since boot at poll-start
  uint32_t free8;         // MALLOC_CAP_8BIT free at poll-start
  uint32_t largest;       // MALLOC_CAP_8BIT largest free block at poll-start
  uint16_t min_free8_64;  // lowest free8 sampled in the cycle, in 64-byte units (see the header)
  uint16_t flags;         // CycleFlag bits
};

// Two hours at 30 s. 240 * 16 = 3,840 B of .bss.
constexpr size_t kCycleLogCap = 240;

// Scale factor for min_free8_64. 64 B units put 4 MB inside a uint16_t with 64 B of resolution,
// which is far finer than anything this measurement is trying to resolve.
constexpr uint32_t kMinFreeScale = 64;

// Opens a new row, filing the one before it. Called from heapTraceBeginCycle(), i.e. once per
// pollOnce(), with the poll-start readings.
void cycleLogBegin(uint32_t free8, uint32_t largest);

// Feeds a free8 sample into the open row's minimum. Safe from any task, cheap enough to call from
// the accept path.
void cycleLogNoteFree8(uint32_t free8);

// ORs bits into the open row's flag word. Safe from any task.
void cycleLogFlag(uint16_t flags);

// Files the open row. Called from the poller's per-cycle stamp - the one path every cycle takes,
// whatever happened inside it - so a cycle that threw is recorded exactly like one that did not.
void cycleLogEnd();

// Total rows filed since boot; also the sequence number the next row will get, so a reader passes
// back what it last saw as `since`.
uint32_t cycleLogSeq();

// Copies out at most `max` rows with sequence number >= `since`, oldest first. Returns how many
// were written and, in *out_first_seq, the sequence number of out[0] - which may be GREATER than
// `since` if the ring wrapped past it, and that gap is itself worth reporting.
size_t cycleLogRead(uint32_t since, CycleLogEntry *out, size_t max, uint32_t *out_first_seq);

}  // namespace transit_app
