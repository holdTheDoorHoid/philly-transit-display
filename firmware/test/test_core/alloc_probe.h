// Test-only: how large the biggest single heap request inside a window was.
//
// It exists for one claim, and it is the claim DESIGN.md SS5's "poll working set" is about: after
// the buffers are reserved once, a poll cycle stops asking the allocator for multi-kilobyte
// CONTIGUOUS blocks. Free heap is not the constraint on the target - the largest free BLOCK is -
// so "how many bytes did a cycle use" is the wrong measurement and "what is the biggest single
// thing it asked for" is the right one.
//
// It works by replacing global operator new/delete (alloc_probe.cpp), which is why the interface
// is a header and the definitions are in exactly one translation unit. Outside a begin()/end()
// window the replacements are a plain malloc/free passthrough, so no other test is affected.
//
// What it does NOT see: ArduinoJson's own allocator, which calls malloc() directly rather than
// operator new. That is deliberate and convenient - a JsonDocument's 1 KB pool chunks are not what
// this is measuring, and the parsers are unchanged by the working set anyway.
#pragma once
#include <cstddef>

namespace transit_test {

struct AllocProbe {
  // Starts a measurement window (and resets the high-water mark).
  static void begin();
  // Ends it and returns the largest single operator new request made inside it, in bytes.
  static size_t end();
  // Allocations counted inside the last window.
  static size_t count();
};

}  // namespace transit_test
