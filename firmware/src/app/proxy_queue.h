// What to do with a job that has just come off the proxy/stats queue - as pure, non-allocating
// arithmetic over two booleans, so the rule can be stated once and tested on the host
// (`pio test -e native -f test_proxy_queue`). Same shape and the same reason as
// poller_liveness.h: the decision used to live inside an `&&` in net_poller.cpp's idle loop, where
// it could not be read or checked.
//
// THE BUG THIS RULE EXISTS FOR (audit_runtime SS4, ranked recommendation 2). The idle loop read
//
//     if (idleWorkHasHeadroom() && runQueuedProxyJob()) { ... }
//
// which short-circuits: on a heap that could no longer clear the gate, the queue was never even
// READ. And a queued job is not inert. proxy_worker.cpp's enqueue() calls
// AsyncWebServerRequest::pause(), which in the pinned library does two things - it returns a
// shared_ptr that keeps the request alive, and it calls client()->setRxTimeout(0), i.e. it turns
// the server-side deadline OFF. So the job sat there for the rest of the device's uptime pinning a
// request object, an AsyncClient and an lwIP pcb, with nothing anywhere able to end it; the client
// waited until it gave up; and once both of the two slots were held, every later
// /api/proxy/stops, /api/proxy/schedule, /api/stats and /api/stats/overview was answered
// "proxy worker busy, try again" - permanently, because the only thing that could free a slot was
// the caller that was gated off.
//
// So the two questions are separated:
//
//   "May I DEQUEUE?"                 always yes. A job that cannot be dealt with must still be
//                                    taken off the queue and ANSWERED, so the slot comes back and
//                                    the paused request is released.
//   "May I START a heavy job?"       a real question - a stats scan allocates a ~9 KB
//                                    StatsAggregator and a Stops proxy streams 400 KB - and when
//                                    the answer is no the job is answered 503, which the shipping
//                                    web UI retries with backoff (DESIGN.md SS10.2).
#pragma once

namespace transit_app {

// What runQueuedProxyJob() does with the job it just took off the queue.
enum class QueuedJobAction {
  Run,             // the client is still there and there is room to work
  DropClientGone,  // nobody to answer: drop it rather than spend seconds of work on it
  Defer,           // not now - put it back at the FRONT and look again next idle slice
  Refuse503,       // waited long enough: answer "low memory, retry" and release the request
};

// How many idle slices a job may be put back before it is answered 503 instead. The idle slice is
// a 250 ms semaphore wait (net_poller.cpp pollerTask), so this is roughly six seconds of waiting -
// deliberately shorter than the ~8 s the device suite gives a request, so the board answers rather
// than letting the client time out. Bounded, so "defer" can never become the old queue hang.
constexpr uint8_t kMaxJobDeferrals = 24;

// `client_alive`: the paused request still has a connected client.
// `may_start_heavy`: the caller says a job's own allocations can be attempted right now - on this
//   firmware that is idleWorkHasHeadroom() AND no burst of web requests in flight (below).
// `deferrals`: how many idle slices this job has already been put back.
//
// WHY "may_start_heavy" GREW A SECOND INPUT (0.3.1-rc3). rc2 asked only the heap gate, which is a
// statement about this instant, and started a ~9 KB StatsAggregator scan whenever free8 was 16 KB
// with a 12 KB block - true at the START of a seven-request burst and false a moment later, once
// those requests had built their documents and send buffers. The device suite crashed there
// (admission.h has the backtrace). A job must not start while a burst is alive, however healthy
// the heap looks at the instant it is asked, so the caller now also requires that essentially
// nothing else is in flight - and a job that cannot start WAITS rather than being refused
// immediately, because the burst is over in seconds and the Stats page would rather be slow than
// wrong.
//
// A dead client still wins over everything: answering it is impossible and dropping it is free.
// "Leave it on the queue indefinitely" is still not an option - Defer is bounded by
// kMaxJobDeferrals and then becomes an answer.
constexpr QueuedJobAction decideQueuedJob(bool client_alive, bool may_start_heavy,
                                           uint8_t deferrals) {
  return !client_alive  ? QueuedJobAction::DropClientGone
         : may_start_heavy ? QueuedJobAction::Run
         : (deferrals < kMaxJobDeferrals ? QueuedJobAction::Defer : QueuedJobAction::Refuse503);
}

}  // namespace transit_app
