// C++ emergency exception pool: why this firmware survives "out of memory while throwing" without
// an SDK rebuild (DESIGN.md SS12.1).
//
// Every long-running task catches std::bad_alloc at its top level (net_poller.cpp, web_server.cpp
// guarded(), main.cpp loop(), sd_logger.cpp), but a catch block only runs if the throw itself can
// be built: __cxa_allocate_exception (libstdc++ eh_alloc.cc) does malloc(96 + sizeof(T)) for the
// exception object plus its __cxa_refcounted_exception header, falls back to a static "emergency
// pool" when that fails, and when the pool is empty too calls std::terminate directly, past every
// try/catch (= reboot). This SDK was built with CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE=0 - no
// pool at all - so the one situation the catch blocks exist for, the heap being gone, was exactly
// the one in which they could not run.
//
// How the pool gets its size, and why this file can set it: eh_alloc.o's static constructor calls
// __cxx_eh_arena_size_get(), which libstdc++ defines WEAK (returning 0), and does one malloc() of
// that many bytes before app_main(); 0 means no pool. ESP-IDF's libcxx.a defines the same function
// STRONG, returning the sdkconfig value, in cxx_init.cpp.obj - the object the Arduino link forces in
// with `-u __cxx_init_dummy`. A plain definition here is therefore a duplicate-symbol link error
// (verified against the real archives). Defining BOTH symbols here is what makes it work with no
// linker flag: this object is on the link line before the SDK archives, so `__cxx_init_dummy` is
// already defined when libcxx.a is scanned, cxx_init.cpp.obj is never pulled in, and eh_alloc.o's
// weak definition yields to ours. The proof is the link map (.pio/build/<env>/firmware.map):
// `.text.__cxx_eh_arena_size_get` comes from this object and cxx_init.cpp.obj appears nowhere.
// (`-Wl,--wrap` cannot do this - the call inside eh_alloc.o is to a symbol that same object
// defines, so it is not an undefined reference for --wrap to redirect; verified, the wrapper is
// simply never referenced. `--allow-multiple-definition` would work, but it silences every
// duplicate symbol in the whole link.)
//
// Cost and size: kCxxExceptionPoolBytes of heap, taken once at static-init while the heap is still
// one contiguous block and never freed - 2 KB of the ~75 KB the firmware runs with. It is touched
// only after malloc has already failed and only while a throw is in flight, so it must hold as many
// exceptions as can be mid-flight at once: the four tasks that allocate (poller, AsyncTCP web
// handlers, LVGL loop, proxy worker), each possibly with a dependent exception from a rethrow.
// pool::allocate() rounds every entry to (16 + 96 + sizeof(T)) rounded up to 16, i.e. 128 B for a
// std::bad_alloc (and for the std::length_error / out_of_range the STL can throw), so 2048 B holds
// 16 - twice the eight that worst case needs. Still uncatchable, by design: C code that gets NULL
// from malloc and does not check it (no throw, so no pool helps), and a catch block that itself
// allocates with nothing left (it rethrows out of the handler). POST /api/debug/oom (web_server.cpp)
// is the deterministic on-device proof; the test suite runs it.
#include "cxx_exception_pool.h"

extern "C" size_t __cxx_eh_arena_size_get(void) { return transit_app::kCxxExceptionPoolBytes; }

// Only here so the SDK's `-u __cxx_init_dummy` is satisfied by this object instead of pulling in
// libcxx.a(cxx_init.cpp.obj), which would bring the SDK's __cxx_eh_arena_size_get along (above).
extern "C" void __cxx_init_dummy(void) {}
