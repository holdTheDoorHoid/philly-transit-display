// The one way a large JSON body leaves this device (DESIGN.md SS12.1 "zero-copy").
//
// Lives in a header rather than in web_server.cpp because it has two callers on two different
// tasks: the web handlers (`/api/state`, `/api/config`, `PUT /api/config`) on the AsyncTCP task,
// and the queued stats jobs (`/api/stats`, `/api/stats/overview`) on the poller task. Those jobs
// used to build a `String` of the whole body with `serializeJson()` and hand it to
// `request->send()`, which is the shape this helper exists to replace - and on the poller task it
// was worse than a memory cost, because serialising on that task is CPU the task watchdog counts
// (cpu_yield.h). Here the document is MOVED into a holder the response owns and serialised on the
// AsyncTCP task, one bounded send-chunk at a time, as the socket drains.
//
// The document is MOVED, not copied, and serialised straight into each TCP send chunk (the filler
// re-walks the document per chunk, skipping what was already sent - the same ChunkPrint
// ESPAsyncWebServer's own AsyncJsonResponse uses). So the response holds the document plus one
// 2 x MSS send buffer - not, as `sendJson()` costs, the document plus a String of the whole body
// plus AsyncBasicResponse's own copy of that String, which needed a body-sized contiguous block
// twice at the worst moment. Chunked transfer encoding, so there is no measuring pass and no
// Content-Length; every client this device has (the web app's fetch(), curl, the device suite)
// handles that.
//
// The per-chunk re-walk is O(chunks^2) in serialisation work, which is only harmless because the
// documents are small: measured on the owner's board 2026-09-16, `/api/state` is 5-8 KB,
// `/api/stats?days=30` is 5,027 B and `/api/stats/overview?days=30` is 5,096 B, i.e. two or three
// 2,872 B chunks. Do not route a document of tens of KB through this without re-checking that -
// the last chunk's filler call would re-serialise nearly the whole document in one go, on the
// AsyncTCP task, which is the starvation this pass was removing rather than moving.
//
// Deliberately NOT used for the small responses: their String is a few dozen bytes, while any
// chunked response allocates the 2.9 KB send buffer (nothrow, retried on the next poll), which is
// the wrong trade for /api/debug/ui, the endpoint the suite uses to watch the device while it is
// starved. Same overflow rule as sendJson(): a truncated document is a 503, never an empty or
// partial 200. Why AsyncJsonResponse itself is not used: it costs two more ArduinoJson serializer
// instantiations (its ChunkPrint-typed fill and measureJson's counting pass, ~2.2 KB of flash) plus
// its class; this serialises through a Print&, which config_store.cpp's two sinks do as well, so
// all three share one instantiation. Measured on cyd-3248S035R: about +0.8 KB of flash for this
// path, against +4.5 KB with AsyncJsonResponse.
#pragma once

#include <ArduinoJson.h>
#include <ChunkPrint.h>  // ESPAsyncWebServer's per-chunk Print sink
#include <ESPAsyncWebServer.h>

#include <memory>
#include <utility>

namespace transit_app {

inline void sendJsonStreamed(AsyncWebServerRequest *request, JsonDocument &doc) {
  if (doc.overflowed()) {
    request->send(503, "application/json", "{\"error\":\"out of memory building the response, retry\"}");
    return;
  }
  std::shared_ptr<JsonDocument> held = std::make_shared<JsonDocument>(std::move(doc));
  request->send(request->beginChunkedResponse("application/json", [held](uint8_t *buf, size_t max_len, size_t index) -> size_t {
    ChunkPrint dest(buf, index, max_len);
    serializeJson(*held, static_cast<Print &>(dest));
    return dest.written();  // 0 once `index` has reached the end of the document = last chunk
  }));
}

}  // namespace transit_app
