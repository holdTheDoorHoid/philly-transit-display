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
  Run,             // the client is still there and there is heap to work with
  DropClientGone,  // nobody to answer: drop it rather than spend seconds of work on it
  Refuse503,       // answer "low memory, retry" now, and release the paused request
};

// `client_alive`: the paused request still has a connected client.
// `may_start_heavy`: the caller's heap gate (net_poller.cpp idleWorkHasHeadroom()) says a job's
// own allocations can be attempted right now.
//
// Note the order: a dead client wins over a low heap, because answering it is impossible and
// dropping it is free. Neither answer is ever "leave it on the queue" - that option is gone.
constexpr QueuedJobAction decideQueuedJob(bool client_alive, bool may_start_heavy) {
  return !client_alive ? QueuedJobAction::DropClientGone
                        : (may_start_heavy ? QueuedJobAction::Run : QueuedJobAction::Refuse503);
}

}  // namespace transit_app
