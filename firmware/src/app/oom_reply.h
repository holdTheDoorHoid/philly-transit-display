// The one way an error reply that exists BECAUSE memory ran out leaves this device.
//
// See heap_reserve.h for the coredump this is written against: guarded() caught a std::bad_alloc
// exactly as designed, and then `request->send(503, ...)` allocated - `beginResponse` does
// `new AsyncBasicResponse(...)`, ~100 B, plus two Strings - that second allocation failed, and a
// throw escaping a catch handler is std::terminate. Every fixed-literal error reply in this
// firmware is on that shape of path, so every one of them goes through here.
//
// Two things happen, in this order, and the order is the point:
//
//   1. The 1 KB reserve is released FIRST, so the response object has somewhere to come from. It
//      is not re-armed here - that would be the handler that just spent it taking it back on the
//      way out, which is precisely when the heap cannot spare it. The poller re-arms it from a
//      quiet moment (net_poller.cpp).
//   2. The send is wrapped in a nested try/catch. A throw inside a catch handler IS catchable by a
//      try block nested inside that handler, so this is legal where an outer guard is not: there
//      is no outer guard on the AsyncTCP path, which is the whole problem. If the send still
//      cannot be built, the connection is closed with no reply and the drop is counted.
//
// A closed connection is a deliberate answer, not a silent failure: the web app's resilientRead()
// (DESIGN.md SS10.2) treats a dropped connection as a retryable failure and backs off, and
// `oom_replies_dropped` on GET /api/debug/ui says how often it happened. Both are better than the
// reboot this replaces.
//
// `body` must be a fixed literal. Anything that has to be formatted first would allocate before
// this function is even entered, which is the bug rather than the fix - sendFailure() in
// web_server.cpp formats into a stack buffer for exactly that reason.
#pragma once

#include <ESPAsyncWebServer.h>

#include <new>

#include "heap_reserve.h"

namespace transit_app {

inline bool sendUnderPressure(AsyncWebServerRequest *request, int code, const char *body) {
  if (request == nullptr) return false;
  releaseHeapReserve();
  try {
    request->send(code, "application/json", body);
    return true;
  } catch (const std::bad_alloc &) {
    noteOomReplyDropped();
    // No reply is possible. Close rather than leave the request open: a paused or unanswered
    // request holds a pcb and (for the queued jobs) has had its server-side timeout switched off,
    // so "do nothing" is the one outcome that does not end.
    AsyncClient *client = request->client();
    if (client != nullptr) client->close();
    return false;
  }
}

}  // namespace transit_app
