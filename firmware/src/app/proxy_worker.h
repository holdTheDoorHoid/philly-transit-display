// Background IO worker task, shared by two DESIGN.md SS7 needs that both take too long to run
// directly on the async web server's own task:
//   GET /api/proxy/stops?route=<route>    -> SEPTA Stops/index.php, verbatim (network)
//   GET /api/proxy/schedule?stop_id=<id>  -> SEPTA BusSchedules/index.php, verbatim (network)
//   GET /api/stats?stop=<key>&days=<n>    -> transit_stats::StatsAggregator over SD (disk IO)
// All three only queue a job here and return immediately; this file's own task does the actual
// work and calls request->send() itself once it has a result, so neither a slow SEPTA round trip
// nor a multi-file SD scan ever blocks other requests or starves LVGL (DESIGN.md SS5).
#pragma once
#include <ESPAsyncWebServer.h>

#include <string>

namespace transit_app {

// Creates the job queue. Call once, early (main.cpp).
void startProxyWorker();

// Takes at most one job off the queue on the calling task (net_poller calls this between polls)
// and disposes of it. Returns false when the queue was empty.
//
// `may_start_heavy` separates two questions that used to be one (audit_runtime SS4, ranked
// recommendation 2). "May I DEQUEUE?" is always yes: a job left on the queue holds a paused
// AsyncWebServerRequest whose server-side timeout the library has switched off, so it waits
// forever, it pins a request object, an AsyncClient and an lwIP pcb, and once both slots are held
// every later stats/proxy request is answered "proxy worker busy" for the rest of the device's
// uptime. "May I START a job that allocates a ~9 KB StatsAggregator?" is a separate question, and
// when the answer is no the job is dequeued and answered 503 - which the client retries - rather
// than left where nothing can reach it. A job whose client has already gone is dropped either way.
bool runQueuedProxyJob(bool may_start_heavy);

// Queues a Stops proxy job for `route` and takes ownership of `request` (the caller must not
// touch it again). Responds 200 with SEPTA's JSON verbatim, or 502 {"error"} on any failure
// (bad status, oversized body, or transport error); guards against the client having
// disconnected before the fetch completes.
void queueStopsProxy(AsyncWebServerRequest *request, const std::string &route);

// Same contract as queueStopsProxy, for BusSchedules by `stop_id`.
void queueScheduleProxy(AsyncWebServerRequest *request, const std::string &stop_id);

// Queues a GET /api/stats job: streams the relevant monthly CSV files (transit_stats::
// monthsInWindow over the last `days` days) through a StatsAggregator for `stop_key` and responds
// with the DESIGN.md SS9.3 JSON shape. Same request-ownership/disconnect-guard contract as above.
void queueStatsRequest(AsyncWebServerRequest *request, const std::string &stop_key, int days);
// Queues a GET /api/stats/overview job: one pass over the same files through an OverviewAggregator
// (every stop and Indego station, DESIGN.md SS9.3).
void queueOverviewRequest(AsyncWebServerRequest *request, int days);

// How many jobs are sitting on the queue right now. GET /api/debug/ui reports it. It used to be
// able to pin at the queue length (2) forever, because the queue was drained only behind a
// free8/largest-block gate a wedged heap could no longer clear; since runQueuedProxyJob() dequeues
// unconditionally that is no longer reachable, and a depth that stays at 2 now means the poller
// itself has stopped running its idle slices.
uint32_t proxyQueueDepth();

}  // namespace transit_app
