#pragma once
// C++ emergency exception pool (DESIGN.md SS12.1) - cxx_exception_pool.cpp explains the mechanism.
#include <cstddef>
#include <cstdint>

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

// The same warming, but for lwIP's OWN task ("tiT"), which we do not create and cannot call into
// directly (0.3.2-rc4). Runs warmExceptionGlobals() on that task through lwIP's callback mailbox
// and blocks until it has finished, so the boot log line is ordered with the others. The .cpp
// explains why it is NOT tcpip_callback_wait() and why the core lock is taken around the post -
// both are traps that would have shipped something worse than the bug.
//
// WHY IT IS NEEDED AT ALL - cxx_exception_pool.cpp has the decoded backtrace. AsyncTCP's lwIP raw
// callbacks (tcp_poll, tcp_recv, tcp_sent, tcp_error, tcp_accept, the DNS callback) all run on the
// tcpip task, and every one of them allocates with `new (std::nothrow)`. libstdc++ implements
// nothrow new as a try/catch around the THROWING new, so a failed allocation there is a real
// std::bad_alloc - the first one on that task, with the heap already at zero, reaching the
// un-warmed __cxa_get_globals malloc and terminating.
//
// Call once at boot, after Wi-Fi is up (the tcpip task does not exist before the stack is
// initialised). Returns true if the warming ran on that task; false if lwIP would not take the
// callback or it did not complete in `timeout_ms`, which is logged and is not fatal.
bool warmExceptionGlobalsOnTcpipTask(uint32_t timeout_ms = 2000);

}  // namespace transit_app

// The hook libstdc++'s eh_alloc.o calls once, before app_main(), to size that pool. Declared here so
// main.cpp can print what the linked definition returns: a real cross-TU call, resolved by the
// linker to the one definition the image carries (cxx_exception_pool.cpp's, per the link map).
// POST /api/debug/oom is the proof the pool actually works; this is only the proof it was asked for.
extern "C" size_t __cxx_eh_arena_size_get(void);
