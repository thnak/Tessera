// SPDX-License-Identifier: MIT
//
// The `net` client: the spec's `send`, its per-request state machine and its cancellation rules
// (docs/specs/networking-v1.md sections 2 and 3; ADR-052; issue #197).
//
// **The state machine** is `NetStateMachine` (`net_state.hpp`), one instance per request:
//
//   Idle --dispatch--> Loading --response--> Done     (any status: a 4xx/5xx is a value)
//                        |-----error------> Error
//                        +--cancel/timeout-> Error
//                        +--redirect------> Loading    (a new hop, same request)
//
// Every state is observable through `status_of`. `Idle` is a real state, not a formality: "never
// blocks the UI thread" is exactly the claim that `submit` returns with the request in `Idle` and
// no I/O started. The terminal outcomes are `NetOutcome`: `Ok`, `HttpStatus` (a non-2xx response)
// and the five failure categories `Timeout`, `Cancelled`, `Transport`, `Limit` and `Request`. That
// split is the difference between a caller that retries (timeout), one that drops silently
// (cancelled) and one that shows an error (transport), so the three are never collapsed into
// "failed".
//
// Terminal is terminal: the first resolution wins, exactly once. A late transport completion for a
// request already resolved by cancellation or timeout is dropped and counted
// (`NetDiagnostics::stale_completions`), which is section 3 item 4's "cancelling before completion
// yields `cancelled` exactly once" and "a cancelled request never delivers a body".
//
// **Cancellation.** Three ways in, all resolving to the same `NetOutcome::Cancelled`, exactly once:
//
//   1. the `CancellationToken` passed to `send`/`submit`, delegated to the attempt's own
//      `CancellationSource` so a real back end observes it and releases its connection;
//   2. `cancel(ticket)`, the explicit per-request form for a screen that unmounts;
//   3. `tick()`, the frame-boundary backstop, which resolves any request whose token is cancelled -
//      so a back end that ignores its token cannot hold a cancelled request past one tick.
//
// **Time.** The section 3 item 3 deadline is a `runtime::TimerQueue` one-shot at
// `clock.now_seconds() + timeout`, so it costs nothing per frame and a test drives it with
// `ManualClock`. Nothing in this file reads a wall clock, sleeps, retries or opens a socket. There
// is deliberately **no retry**: section 3 item 3 says "There is no implicit retry", and
// async-data-v1.md section 3 owns backoff policy (`http_method_is_idempotent` is the fact that
// policy branches on).
//
// **The spec's signature.** Section 2 writes
// `send(Request, CancelToken) -> Task<std::expected<Response, NetError>>`; `send` here is that, in
// the only shape `runtime::Task` can express (its promise takes the `TaskContext` first, so a
// member coroutine body is impossible - docs/design/services.md section 1.1). The returned task's
// own token is deliberately empty: cancelling it would make the runtime throw `TaskCancelled` at
// the `co_await`, and section 2 wants `cancelled` **as a value** in the `expected`. Cancellation
// therefore flows through the token the caller passes, not through the task frame.
//
// **Not in this slice**, named in docs/design/networking-module.md section 7: every real back end
// (WinHTTP, libcurl, `fetch`, NSURLSession, Android) needs `docs/specs/dependencies.md` entries
// before it can land, and none is linked here; streamed response bodies; and WebSocket (a separate
// interface, section 1's non-goals). TLS and HTTP/2 are negotiated *inside* a back end, so nothing
// in this file changes when they land behind the seam.

#pragma once

#include "tessera/appfw/http_service.hpp"
#include "tessera/appfw/net_state.hpp"
#include "tessera/appfw/net_transport.hpp"
#include "tessera/appfw/net_types.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/io_task_executor.hpp"
#include "tessera/runtime/task.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

namespace detail {
// The client's implementation: records, attempts and delivery. Defined in
// `src/appfw/net_client_internal.hpp` so the public header carries no implementation detail.
class NetClientImpl;
} // namespace detail

// A copyable snapshot of one request's state: what `status_of` returns, deliberately small (a
// status query must never copy a 16 MiB body).
struct NetRequestStatus {
    NetTicket ticket{};
    // False for an unknown, evicted or released ticket: `state` is then meaningless, which is a
    // different fact from "the request is Idle".
    bool known = false;
    NetRequestState state = NetRequestState::Idle;
    NetOutcome outcome = NetOutcome::None;
    std::uint32_t attempts = 0; // 1 plus the number of followed redirects
    int status = 0;             // the last response's status; 0 when there was none
    std::optional<NetErrorKind> error_kind;
    std::size_t body_bytes = 0;

    [[nodiscard]] bool terminal() const noexcept {
        return state == NetRequestState::Done || state == NetRequestState::Error;
    }
};

// One recorded failure. Diagnostics are values and bounded (ADR-022): the last N entries plus
// counters, never a log line per request. `detail` is always built through `redacted_url` when it
// names a URL, so a query string or a userinfo cannot reach a log line (section 3 item 9).
struct NetDiagnostic {
    NetErrorKind kind = NetErrorKind::Other;
    std::string detail;
};

struct NetDiagnostics {
    std::size_t recorded = 0;
    std::size_t dropped = 0;
    std::size_t capacity = 0;
    std::vector<NetDiagnostic> entries;

    std::uint64_t submitted = 0;
    std::uint64_t completed = 0; // a response arrived, of any status
    std::uint64_t failed = 0;    // an error arrived
    std::uint64_t cancelled = 0;
    std::uint64_t timed_out = 0;
    std::uint64_t redirects = 0;
    std::uint64_t refused = 0;           // at the in-flight bound, or a cleartext URL
    std::uint64_t stale_completions = 0; // a late result for an already-resolved request
};

// Everything the client is configured with, all of it explicit (services.md section 1: "no
// environment-variable magic in production").
struct NetClientConfig {
    // Bounded concurrency (services.md section 5). A `submit` beyond it is refused rather than
    // queued: the caller chose how many screens may fetch at once.
    std::size_t max_in_flight = 8;
    // Terminal records kept so `status_of`/`response_of` keep answering. The oldest terminal record
    // is evicted first; a request in flight is never evicted.
    std::size_t max_records = 32;
    // Section 3 item 8: plain http is allowed only when the app enables it. A cleartext request
    // with this false is `NetErrorKind::Unsupported` and never reaches the transport.
    bool allow_cleartext = false;
    // Section 3 item 7: "the library adds no `User-Agent` beyond a fixed `Tessera/<version>`". An
    // empty `user_agent` means that fixed value; `send_default_user_agent = false` means the client
    // adds nothing at all, and a caller that sets its own is never second-guessed.
    bool send_default_user_agent = true;
    std::string user_agent;
    // How many failures the diagnostics ring keeps. 0 disables the ring but keeps the counters.
    std::size_t diagnostic_capacity = 16;
};

// One per app (or per window) scope. Borrowed dependencies, every one of them explicit:
//
//   * `transport` - the back end (section 4): `FakeNet` in tests and headless runs;
//   * `executor` - where an attempt is *started*, so a back end that blocks internally still never
//     runs on the UI thread (AF7's `IoTaskExecutor`; `ManualIoExecutor` in tests);
//   * `completions` - the queue the returned `Task` and every callback run on: the UI thread;
//   * `timers` - where the per-attempt deadline lives (AF2's layer-0 wake source);
//   * `clock` - the only time source (section 3 item 10).
//
// All five must outlive the client. Non-movable: a ticket names a record whose address must not
// change, and the impl is shared with the in-flight attempts so a destroyed client cannot dangle.
class NetClient final : public HttpService {
public:
    // What `submit` delivers, on the thread that drains the completion queue. Exactly one call per
    // accepted `submit`, unless the client is destroyed first (then it is not called at all, and
    // the request simply ends with the client).
    using Callback = HttpService::Callback;

    NetClient(NetClientConfig config, NetTransport& transport, runtime::IoTaskExecutor& executor,
              runtime::CompletionQueue& completions, runtime::TimerQueue& timers,
              runtime::Clock& clock);
    ~NetClient() override;

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;
    NetClient(NetClient&&) = delete;
    NetClient& operator=(NetClient&&) = delete;

    // Section 2's `send`, in the shape services.md section 1.1 documents. The task's own token is
    // empty on purpose; `token` is the cancellation input. The coroutine is lazy: nothing happens
    // until it is started or awaited.
    [[nodiscard]] runtime::Task<NetResult> send(NetRequest request,
                                                runtime::CancellationToken token) override;

    // The coroutine-free form, for a caller that is not itself a coroutine (a frame tick, an event
    // handler). Returns the ticket when the request was accepted, or the error when it was refused
    // before anything started (`bad_url`, cleartext, unsupported back end, at the in-flight bound)
    // - in which case `on_done` is never invoked.
    //
    // `on_done` runs exactly once, on the thread that drains the completion queue. It may be empty,
    // and it must not call `submit` re-entrantly from a resolution it caused: a redirect is handled
    // internally, never by recursion.
    [[nodiscard]] std::expected<NetTicket, NetError>
    submit(NetRequest request, runtime::CancellationToken token, Callback on_done) override;

    // Explicit per-request cancellation (section 3 item 4). Resolves the request as `cancelled`,
    // cancels the attempt's token so the back end releases its connection, cancels the deadline and
    // invokes the callback once. False for an unknown or already-terminal ticket, which is what
    // makes a second cancel a no-op instead of a second delivery.
    bool cancel(NetTicket ticket) override;

    // The frame-boundary drive point: resolves every request whose token was cancelled, so a back
    // end that ignores its token cannot hold a cancelled request past one tick. Cheap and
    // allocation-free when there is nothing to do, so a host calls it once per tick, after the
    // completion drain.
    void tick();

    // Frees a terminal record early, so `max_records` need not grow for a long-lived app. False for
    // an unknown ticket or one still in flight.
    bool release(NetTicket ticket);

    [[nodiscard]] NetRequestStatus status_of(NetTicket ticket) const;
    // The response/error of a terminal request, or null. Borrowed: valid until the record is
    // evicted, released, or the client destroyed.
    [[nodiscard]] const NetResponse* response_of(NetTicket ticket) const;
    [[nodiscard]] const NetError* error_of(NetTicket ticket) const;

    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] std::size_t records() const noexcept;
    [[nodiscard]] const NetClientConfig& config() const noexcept;
    // Non-copyable on purpose: a diagnostics snapshot builds a fresh ring and copies the counters,
    // so it must be assigned to a variable, not returned through a reference.
    [[nodiscard]] NetDiagnostics diagnostics() const;
    [[nodiscard]] std::string_view transport_name() const noexcept;

private:
    // The coroutine body, context-first because `runtime::Task`'s promise consumes the context
    // there (docs/design/services.md section 1.1).
    static runtime::Task<NetResult> send_body(runtime::TaskContext context, NetClient& self,
                                              NetRequest request, runtime::CancellationToken token);

    std::shared_ptr<detail::NetClientImpl> impl_;
};

} // namespace tessera::appfw
