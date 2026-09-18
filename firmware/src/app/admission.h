// Whether a brand-new TCP connection may become an AsyncWebServerRequest at all - decided at
// ACCEPT time, before anything is allocated for it, as pure arithmetic over three numbers so it
// can be host-tested (`pio test -e native -f test_admission`). Same shape as poller_liveness.h,
// proxy_queue.h and heap_reserve.h; the machinery that measures the inputs lives in web_server.cpp
// because it needs the library.
//
// WHY THIS EXISTS: THE FOURTH UNCATCHABLE-OOM INSTANCE (device suite, 0.3.1-rc2, section E).
// Three rounds of seven concurrent requests (4x /api/state + /api/proxy/stops + /api/stats + one
// asset) crashed the board. Decoded backtrace, task async_tcp:
//
//   abort <- std::terminate <- __cxa_throw <- operator new
//     <- std::list<AsyncWebHeader>::emplace_back
//     <- AsyncWebServerResponse::addHeader <- AsyncAbstractResponse::_assembleHead
//     <- AsyncWebServerRequest::_respond <- _send <- _parseLine <- _onData <- AsyncClient::_recv
//
// Read it against what DESIGN.md SS12.1 already documents and the difference is the whole point:
// the response object handed to request->send() is only STORED. The library assembles the header
// list LATER, when the request completes, on a library frame inside _parseLine with no handler of
// ours above it. So the 1 KB reserve (released just before send()) has already been spent by then,
// and oom_reply.h's nested try/catch is nowhere on that stack. Neither defence can reach it.
//
// Nothing we can write catches an allocation the library makes on its own frames. The only lever
// left is to not have that many requests in flight at once - admission control, at accept, where a
// refusal costs zero allocations because the connection object is destroyed before a request,
// a response, a header list or a send buffer is ever built.
#pragma once
#include <cstddef>
#include <cstdint>

namespace transit_app {

// What one in-flight request costs before it is answered: AsyncWebServerRequest 356 B +
// AsyncClient 196 B + lwIP tcp_pcb 208 B + the parsed url/host/header Strings + the response
// object, and then the chunked send buffer (ASYNC_RESPONCE_BUFF_SIZE = CONFIG_LWIP_TCP_MSS * 2 =
// 2,872 B) - about 5 KB in all (audit_static SS5, "AsyncWebServer / AsyncTCP objects").
constexpr size_t kRequestCostBytes = 5028;
constexpr size_t kSendBufferBytes = 2872;

// Refuse a new connection below this much MALLOC_CAP_8BIT free: enough for the connection we are
// about to admit to be answered at all. Deliberately NOT a round number or a multiple of 512 - it
// is the sum above, not a threshold picked for looking tidy, and DESIGN.md SS2.1 explains why a
// round figure lands badly against real heap readings.
constexpr size_t kAcceptMinFree8 = kRequestCostBytes + kSendBufferBytes;  // 7,900

// ...and below this largest contiguous block. The biggest single piece a request needs is its send
// buffer; 1.5x it leaves room for the header list and the response object beside it.
constexpr size_t kAcceptMinLargestBlock = 4308;  // 1.5 x 2,872, rounded off the 512-byte lattice

// Hard cap on requests in flight at once.
//
// THE NUMBER IS A TRADE, AND THE SUITE CONSTRAINS IT. Section E fires 7 concurrent requests per
// round for 3 rounds and allows at most SIX refusals in total ("E at most 2 refusals per round",
// exhaustive_test.py). A cap of N refuses about (7 - N) per round once the burst is simultaneous,
// so 3 rounds x (7 - N) <= 6 needs N >= 5. A cap of 3 would refuse ~12 and fail that check by
// construction, however well it protected the heap.
//
// AND SINCE 0.3.2-rc3 THE CAP IS CARRYING THAT BUDGET ON ITS OWN, which is what makes the
// arithmetic above exact rather than a lower bound. While the floors applied from the second
// request they could refuse on top of the cap, and rc5's section E measured nine refusals against
// the budget of six for precisely that reason. With kAdmissionFloorsApplyFrom at 2 a seven-deep
// simultaneous burst loses (7 - 5) = 2 per round to the cap and nothing else, so 3 x 2 = 6.
//
// 5 is therefore the tightest cap the acceptance criteria allow, and it is not carrying the whole
// defence on its own: the free-heap floor above refuses earlier when the heap is genuinely short,
// refuseIfLowHeap() still turns heavy handlers into cheap 503s, and the queued stats/proxy jobs no
// longer start while a burst is alive (proxy_queue.h). Worth knowing: that check's own comment says
// "Not connection-count exhaustion: nothing here caps concurrent clients", which was true when it
// was written and is not any more - if a run shows more than six refusals, the honest fix is to
// revisit the budget with the owner, not to loosen the floors.
constexpr uint32_t kMaxInFlightRequests = 5;

// How many requests must ALREADY be in flight before the heap floors have anything to say.
//
// TWO since 0.3.2-rc3, i.e. the floors gate the THIRD concurrent request onward. It was one - the
// floors applied from the second - and the device suite's section E measured what that costs:
// NINE aborted connections across three rounds against a budget of six. The floors were refusing
// the second request of a burst whenever the heap was down, which on a board mid-poll it often
// is, and a refusal there buys very little: two requests alive at once is not the state that
// crashed rc2. That needed SEVEN, each holding a document, a response object and a 2,872 B send
// buffer, and the count cap - not the floors - is what stops a burst reaching seven.
//
// So the floors keep doing the job they were derived for (refusing the request that arrives into
// genuine contention) and stop doing the job the cap already does (bounding the burst). A second
// concurrent request on a starved heap is still not free: if its own reply will not fit,
// guarded() catches the throw and answers 503 out of the 1 KB reserve (heap_reserve.h), exactly
// as it does for the first.
constexpr uint32_t kAdmissionFloorsApplyFrom = 2;

// THE GUARANTEED SERVICE LEVEL: TWO REQUESTS AT A TIME, ALWAYS (0.3.2-rc1, widened in rc3).
//
// What rc3's rule did, measured on the owner's board at v0.3.1 on 2026-09-17 (uptime 2,646 s):
// the heap fragmented until the largest free block was 3,444 B, which is below
// kAcceptMinLargestBlock, and from that moment the accept path refused EVERY new connection. The
// board answered ping and refused all HTTP - including `GET /api/debug/ui`, the one endpoint
// deliberately kept outside refuseIfLowHeap() precisely so it still answers when the heap is gone
// (web_server.cpp), and `POST /api/reboot`, which is the recovery path. The only way out was the
// heap-wedge self-heal, five minutes later, with nobody able to even look at the device in the
// meantime.
//
// That is a worse failure than the one admission control exists to prevent. The crash it prevents
// needs SEVERAL requests alive at once: rc2 died with seven of them holding documents, response
// objects and 2,872 B send buffers. ONE request cannot reproduce it - there is no burst, nothing
// else is about to allocate, and if that single request's own reply cannot be built, guarded()
// catches the throw and answers 503 out of the 1 KB reserve (heap_reserve.h). So the heap floors
// are a statement about CONTENTION, and with no contention they have nothing to say.
//
// Hence: the floors apply only from kAdmissionFloorsApplyFrom requests already in flight - two
// since 0.3.2-rc3, so a browser that opens a second connection while the first is still being
// answered is admitted whatever the heap says. The count cap is unchanged and still hard at
// kMaxInFlightRequests.
//
// WHAT THE SUITE MEASURED, which is why this moved (0.3.2-rc3). Section E fires 7 concurrent
// requests per round for 3 rounds and allows at most SIX aborted connections in total. rc5
// produced NINE: the cap accounts for 2 per round (7 - 5), and the extra three were the floors
// refusing a SECOND request on a heap that was down - which is not the state the floors exist
// for. At the new threshold the arithmetic is the cap's alone: ~2 refusals per round, 6 total,
// inside the budget by construction rather than by luck. If a run still exceeds it, the honest
// fix is to revisit the budget with the owner, not to loosen the floors further.
//
// The rule table, which is what test_admission pins:
//
//   in_flight == 0                      -> admit, whatever the heap says
//   in_flight == 1                      -> admit, whatever the heap says (rc3)
//   2 <= in_flight < cap, heap ok       -> admit
//   2 <= in_flight < cap, either floor  -> refuse (the burst defence)
//   in_flight >= cap                    -> refuse, whatever the heap says (unchanged)
//
// `in_flight` is how many AsyncWebServerRequest objects are alive right now; `free8`/`largest` are
// MALLOC_CAP_8BIT readings taken on the accept path.
constexpr bool admitConnection(uint32_t in_flight, size_t free8, size_t largest) {
  if (in_flight >= kMaxInFlightRequests) return false;
  // Not yet contention. Refusing here buys little - the crash the floors exist for needed seven
  // requests alive at once - and costs the device its diagnostics and its reboot endpoint at
  // exactly the moment somebody is trying to look at it.
  if (in_flight < kAdmissionFloorsApplyFrom) return true;
  return free8 >= kAcceptMinFree8 && largest >= kAcceptMinLargestBlock;
}

}  // namespace transit_app
