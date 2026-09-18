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
// exceptions as can be mid-flight at once: the three tasks that allocate (the poller task, which
// also runs the queued proxy and stats jobs; the AsyncTCP web handlers; the LVGL display loop),
// each possibly with a dependent exception from a rethrow. pool::allocate() rounds every entry to
// (16 + 96 + sizeof(T)) rounded up to 16, i.e. 128 B for a std::bad_alloc (and for the
// std::length_error / out_of_range the STL can throw), so 2048 B holds 16 - well over the six that
// worst case needs. POST /api/debug/oom (web_server.cpp) is the deterministic on-device proof; the
// test suite runs it, on a fresh boot, for the reason below.
//
// ---------------------------------------------------------------------------------------------
// The pool is not sufficient on its own: a task's FIRST throw allocates, outside the pool
// (found 2026-09-16, and the reason warmExceptionGlobals() exists).
//
// __cxa_throw's first act is __cxa_get_globals() (libstdc++ eh_globals.cc), which keeps the
// per-thread "exception in flight" bookkeeping - __cxa_eh_globals, the caught/uncaught counters the
// unwinder and every catch block read - in thread-local storage:
//
//     g = __gthread_getspecific(key);            // NULL the first time this task ever throws
//     if (!g) {
//       g = malloc(sizeof(__cxa_eh_globals));    // a PLAIN malloc - the emergency pool is
//       if (!g || __gthread_setspecific(...))    //   __cxa_allocate_exception's, not this one
//         std::terminate();                      // <- unconditional, past every try/catch
//     }
//
// That malloc is ~16 bytes and happens once per task, but on a task's first throw it happens at the
// worst possible moment: a first throw is overwhelmingly a bad_alloc, i.e. the heap is already gone,
// so the malloc fails and std::terminate() runs before __cxa_allocate_exception is ever reached -
// the pool is never consulted, and the catch block never runs. Observed exactly so: a fresh boot,
// the first request being POST /api/debug/oom (the pool's own proof), aborted the device -
//   abort <- __terminate <- __cxa_get_globals (eh_globals.cc:150) <- __cxa_throw <- operator new[]
//   <- handleDebugOom <- ... <- _async_service_task
// - while the full device suite passed, because by then something earlier had already thrown on the
// AsyncTCP task and paid the allocation while the heap was healthy.
//
// The fix is to pay it deliberately, early: every task that can throw calls warmExceptionGlobals()
// while the heap is plentiful, so __gthread_getspecific is non-NULL from then on and the malloc
// above can never be reached under pressure. It cannot be done once, centrally - the storage is
// per-task, so each task must warm itself, on itself. Call sites: main.cpp setup() (loopTask, which
// is also the LVGL display loop), net_poller.cpp pollerTask() (which also runs the queued proxy and
// stats jobs), and web_server.cpp's first-thing middleware for the AsyncTCP task, which the library
// creates and we therefore cannot warm at its entry. The short-lived "restart" task (web_server.cpp)
// only sleeps and reboots and never throws.
//
// ---------------------------------------------------------------------------------------------
// AND THE FOURTH TASK, WHICH IS NOT OURS AND HAS BEEN UNCOVERED IN EVERY RELEASE UNTIL NOW:
// lwIP's "tiT" (found 0.3.2-rc4, from the rc3 device-suite serial capture).
//
// Decoded backtrace, core 0, during section A's POST /api/debug/oom on a cold boot - i.e. with the
// heap deliberately taken to zero:
//
//   tcpip_thread <- sys_check_timeouts <- tcp_slowtmr
//     <- AsyncTCP_detail::tcp_poll (AsyncTCP.cpp:454)
//     <- operator new(nothrow) <- operator new <- __cxa_throw
//     <- __cxa_get_globals <- std::terminate
//
// Read the middle of that and the trap is plain: **libstdc++ implements nothrow new as a try/catch
// around the THROWING new** -
//
//     void* operator new(size_t sz, const nothrow_t&) noexcept {
//       __try { return ::operator new(sz); } __catch(...) { return nullptr; }
//     }
//
// - so `new (std::nothrow) T` is not a non-throwing allocation at all. It is a throw and a catch,
// and it needs the calling task's __cxa_eh_globals exactly like any other throw. AsyncTCP allocates
// that way in every one of its lwIP raw callbacks (verified in AsyncTCP.cpp: tcp_poll:454,
// tcp_recv:468, tcp_sent:493, tcp_error:516, the DNS callback:531, tcp_connected:429, and
// tcp_accept:1577/1581 - the last of which allocates an AsyncClient), and lwIP runs every one of
// those on the tcpip task. So the FIRST allocation failure anywhere in AsyncTCP's lwIP half hits
// the un-warmed __cxa_get_globals malloc, with the heap by definition already gone, and terminates.
//
// Nothing of ours runs on that task, which is why this was invisible: the trap is entirely in
// library code, reached through a library callback, and the only thing we can do about it is make
// sure the task is warm before it ever gets there. warmExceptionGlobalsOnTcpipTask() does that by
// posting warmExceptionGlobals() to lwIP with tcpip_callback_wait(), which runs it ON the tcpip
// task and blocks until it has finished.
//
// It has been present in every release. rc3 is only the first build whose heap gate let
// POST /api/debug/oom exhaust the heap completely enough to reach it.
//
// WHAT WAS CHECKED AND DELIBERATELY NOT WARMED. `esp_timer`: AsyncTCP touches esp_timer only for
// esp_timer_get_time() (its millis()/micros()), never a callback, and this firmware registers no
// esp_timer callbacks at all. `sys_evt` / the Arduino event task: this firmware registers no
// WiFi.onEvent() handler and no esp_event handler, so nothing of ours can throw there. Warming
// either would be warming a task on which no C++ throw exists - speculation rather than a fix.
// **If a future change registers a callback on any task not listed above, that task must warm
// itself, and this is the paragraph it should be added to.**
#include "cxx_exception_pool.h"

#include <Arduino.h>
#include <cxxabi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/tcpip.h>

extern "C" size_t __cxx_eh_arena_size_get(void) { return transit_app::kCxxExceptionPoolBytes; }

// Only here so the SDK's `-u __cxx_init_dummy` is satisfied by this object instead of pulling in
// libcxx.a(cxx_init.cpp.obj), which would bring the SDK's __cxx_eh_arena_size_get along (above).
extern "C" void __cxx_init_dummy(void) {}

namespace transit_app {
namespace {

// The throw is in its own noinline function on purpose. GCC folds a `try { throw 0; } catch (int)`
// whose handler it can see into a plain jump - no __cxa_throw call at all - which would warm
// nothing while looking exactly like it did. Across a noinline call boundary it cannot, so the real
// __cxa_throw path runs. (`noclone` stops IPA making a specialised copy it can then fold.)
// Verified on the built image: `xtensa-esp-elf-objdump -d` shows the call to __cxa_throw inside it.
[[gnu::noinline, gnu::noclone]] void throwOnce() {
  volatile int tag = 0;  // volatile so the thrown value is not a compile-time constant
  throw (int)tag;
}

}  // namespace

bool warmExceptionGlobals(const char *task_name) {
  // Both ABI entry points are declared __attribute__((const)) in <cxxabi.h>, so GCC will happily
  // delete a call whose result it sees go unused. Every result below is consumed - branched on, or
  // fed to an asm barrier - so the calls actually survive into the image.
  if (abi::__cxa_get_globals_fast() != nullptr) return false;  // this task has thrown before

  // Allocates and installs the per-thread __cxa_eh_globals: the fix proper, and independent of
  // whether the throw below survives the optimiser.
  void *globals = static_cast<void *>(abi::__cxa_get_globals());
  asm volatile("" : : "r"(globals) : "memory");

  // Then a real throw, which is worth the few microseconds it costs once per task: it proves the
  // whole path works on this task rather than only that the allocator was called, and it forces the
  // one other first-throw-only allocation in the chain - GCC's unwinder sorting the FDE lookup
  // table on first use (unwind-dw2-fde.c init_object(), a process-wide malloc) - to happen here,
  // with the heap healthy, instead of during an OOM. (That one degrades to a linear search rather
  // than terminating, so it is a latency and determinism fix, not a crash fix.)
  bool threw = false;
  try {
    throwOnce();
  } catch (int) {
    threw = true;
  }

  Serial.printf("[heap] eh_globals warmed on %s (globals=%p, throw path %s, free %u)\n",
                task_name ? task_name : "?", globals, threw ? "ok" : "MISSING",
                (unsigned)ESP.getFreeHeap());
  return true;
}

namespace {

// FILE-SCOPE and not a stack struct, deliberately. tcpip_callback_wait() blocks until the callback
// has run, so a local would normally be safe - but if the wait ever gained a timeout or lwIP
// changed its contract, a callback arriving late would write through a dangling pointer on a task
// we do not control. This runs exactly once at boot, so one static costs nothing and cannot dangle.
volatile bool g_tcpip_warm_ran = false;

void warmOnTcpipTask(void *) {
  // Whatever lwIP's tcpip task is called in this build - CONFIG_LWIP_TCPIP_TASK_NAME, "tiT" by
  // default. The name is only for the log line; the warming applies to whichever task actually runs
  // this callback, which is the tcpip task by construction.
  warmExceptionGlobals("tiT");
  g_tcpip_warm_ran = true;
}

}  // namespace

bool warmExceptionGlobalsOnTcpipTask(uint32_t timeout_ms) {
  (void)timeout_ms;  // tcpip_callback_wait() blocks until the callback has run; see below.
  g_tcpip_warm_ran = false;
  // tcpip_callback_wait() and not tcpip_callback()/tcpip_try_callback(): the other two return as
  // soon as the message is QUEUED, which would let boot race on past this and print the line out of
  // order with the other three warmings - and, worse, would make "it was warmed" unverifiable.
  // _wait blocks the caller (loopTask, in setup()) until the callback has returned on tiT.
  const err_t e = tcpip_callback_wait(warmOnTcpipTask, nullptr);
  if (e != ERR_OK) {
    // Not fatal, and not silent. The likeliest cause is being called before the TCP/IP stack is up,
    // which is a call-site ordering bug rather than a runtime condition.
    Serial.printf("[heap] eh_globals NOT warmed on tiT: tcpip_callback_wait() = %d\n", (int)e);
    return false;
  }
  if (!g_tcpip_warm_ran) {
    Serial.println("[heap] eh_globals NOT warmed on tiT: callback did not run");
    return false;
  }
  return true;
}

}  // namespace transit_app
