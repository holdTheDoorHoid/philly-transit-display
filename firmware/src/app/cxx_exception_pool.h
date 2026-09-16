#pragma once
// C++ emergency exception pool (DESIGN.md SS12.1) - cxx_exception_pool.cpp explains the mechanism.
#include <cstddef>

namespace transit_app {

// Bytes libstdc++ reserves at static-init for exception objects it cannot malloc. 128 B per
// std::bad_alloc; cxx_exception_pool.cpp has the arithmetic behind the number.
constexpr size_t kCxxExceptionPoolBytes = 2048;

}  // namespace transit_app

// The hook libstdc++'s eh_alloc.o calls once, before app_main(), to size that pool. Declared here so
// main.cpp can print what the linked definition returns: a real cross-TU call, resolved by the
// linker to the one definition the image carries (cxx_exception_pool.cpp's, per the link map).
// POST /api/debug/oom is the proof the pool actually works; this is only the proof it was asked for.
extern "C" size_t __cxx_eh_arena_size_get(void);
