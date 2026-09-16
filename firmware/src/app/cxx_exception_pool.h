#pragma once
// C++ emergency exception pool (DESIGN.md SS12.1) - cxx_exception_pool.cpp explains the mechanism.
#include <cstddef>

namespace transit_app {

// Bytes libstdc++ reserves at static-init for exception objects it cannot malloc. 128 B per
// std::bad_alloc; cxx_exception_pool.cpp has the arithmetic behind the number.
constexpr size_t kCxxExceptionPoolBytes = 2048;

// Does, on the CALLING task and while the heap is still plentiful, the one-time allocation that
// libstdc++ would otherwise do inside the task's *first* throw - where it is unconditionally fatal.
// Must be called from each task that can throw, early: the pool above does not cover this one
// (cxx_exception_pool.cpp has the whole story). Cheap and idempotent - a task that is already warm
// pays one pthread_getspecific - so it is safe on a hot path like the web middleware.
// Returns true if THIS call did the warming, false if the task was already warm; logs one line
// when it warms, tagged with task_name.
bool warmExceptionGlobals(const char *task_name);

}  // namespace transit_app

// The hook libstdc++'s eh_alloc.o calls once, before app_main(), to size that pool. Declared here so
// main.cpp can print what the linked definition returns: a real cross-TU call, resolved by the
// linker to the one definition the image carries (cxx_exception_pool.cpp's, per the link map).
// POST /api/debug/oom is the proof the pool actually works; this is only the proof it was asked for.
extern "C" size_t __cxx_eh_arena_size_get(void);
