// SPDX-License-Identifier: MIT
//
// `NetClient`'s implementation, kept out of the public header (issue #197). The module's public
// surface is the four headers a caller includes; this one is only for the client's own translation
// units.
//
// Threading, stated once because every method depends on it: **everything here runs on the thread
// that drains the completion queue** - the UI thread - except the one `IoTask.work` body, which
// only hands the request to the transport and touches no shared state. A transport's `Completion`
// is invoked on that UI thread (net_transport.hpp contract 1), so no lock is taken anywhere in this
// class.

#pragma once

#include "tessera/appfw/net_client.hpp"
#include "tessera/appfw/net_redirect.hpp"
#include "tessera/appfw/net_state.hpp"
#include "tessera/appfw/net_transport.hpp"
#include "tessera/appfw/net_types.hpp"
#include "tessera/appfw/net_url.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/io_task_executor.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw::detail {

// One submitted request: its state machine, the hop currently outstanding, and what it owes the
// caller. A record is alive from `submit` until it is evicted or released; a transport completion
// only carries a ticket, and a stale ticket finds nothing rather than a dangling record.
struct NetRecord {
    NetTicket ticket{};
    NetStateMachine machine;
    // The current hop: rewritten by a followed redirect, so the back end sees the real chain and
    // `response.final_url` is the last hop's URL.
    NetRequest request;
    NetResponse response;
    NetError error;

    // What the caller is owed, exactly once. Empty is legal (a caller that polls `status_of`).
    NetClient::Callback callback;

    // The caller's token, polled by `tick()` so a back end that ignores its own token cannot keep a
    // cancelled request alive past one frame.
    runtime::CancellationToken caller_token;
    // The token handed to the transport for the current hop. Cancelling it on resolution is what
    // releases the connection (section 3 item 4).
    runtime::CancellationSource attempt_token;

    // Section 3 item 3's per-attempt deadline, on the injected clock.
    double deadline_seconds = 0.0;
    runtime::TimerId deadline_timer{};
    bool timer_armed = false;
};

class NetClientImpl final : public std::enable_shared_from_this<NetClientImpl> {
public:
    NetClientImpl(NetClientConfig config, NetTransport& transport,
                  runtime::IoTaskExecutor& executor, runtime::CompletionQueue& completions,
                  runtime::TimerQueue& timers, runtime::Clock& clock);
    ~NetClientImpl();

    NetClientImpl(const NetClientImpl&) = delete;
    NetClientImpl& operator=(const NetClientImpl&) = delete;

    [[nodiscard]] std::expected<NetTicket, NetError>
    submit(NetRequest request, runtime::CancellationToken token, NetClient::Callback callback);

    bool cancel(NetTicket ticket);
    void tick();
    bool release(NetTicket ticket);

    [[nodiscard]] NetRequestStatus status_of(NetTicket ticket) const;
    [[nodiscard]] const NetResponse* response_of(NetTicket ticket) const;
    [[nodiscard]] const NetError* error_of(NetTicket ticket) const;

    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] std::size_t records() const noexcept {
        return records_;
    }
    [[nodiscard]] const NetClientConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] NetDiagnostics diagnostics() const;
    [[nodiscard]] std::string_view transport_name() const noexcept;
    [[nodiscard]] runtime::CompletionQueue& completions() const noexcept {
        return *completions_;
    }

private:
    struct Slot {
        std::unique_ptr<NetRecord> record;
        std::uint32_t generation = 0;
    };

    [[nodiscard]] NetRecord* find(NetTicket ticket) noexcept;
    [[nodiscard]] const NetRecord* find(NetTicket ticket) const noexcept;
    [[nodiscard]] Slot* slot_of(NetTicket ticket) noexcept;
    [[nodiscard]] const Slot* slot_of(NetTicket ticket) const noexcept;
    // A slot for a new record, evicting the oldest terminal record when the bound is reached. Null
    // when every record is in flight.
    [[nodiscard]] Slot* acquire_slot();
    void dispose(Slot& slot) noexcept;

    // Queues the current hop on the executor. False when the executor refused the submission.
    bool dispatch(NetTicket ticket);
    // The executor's completion: `accepted` is the transport's own verdict, reported on the UI
    // thread. A refusal resolves the request here, because no transport completion will ever come.
    void on_started(NetTicket ticket, bool accepted);
    // The transport's completion, on the UI thread.
    void on_transport_result(NetTicket ticket, NetResult result);
    // Follows a 3xx when section 3 item 2 says to, or resolves the response as it stands.
    void handle_response(NetRecord& record, NetResponse response);
    // Resolves a record: the terminal transition, the connection release, the deadline cancel and
    // the single delivery. False when the record was already terminal (a stale completion).
    bool resolve(NetTicket ticket, NetResult result);
    // Resolves through the completion queue, so a failure discovered *inside* `submit` still
    // delivers asynchronously - the caller has not yet been handed the ticket, and a callback
    // before that would be a re-entrancy hazard for no benefit.
    void resolve_later(NetTicket ticket, NetError error);
    void deliver(NetRecord& record);

    void arm_deadline(NetRecord& record);
    void disarm_deadline(NetRecord& record);
    void record_diagnostic(NetErrorKind kind, std::string_view detail);

    NetClientConfig config_;
    NetTransport* transport_ = nullptr;
    runtime::IoTaskExecutor* executor_ = nullptr;
    runtime::CompletionQueue* completions_ = nullptr;
    runtime::TimerQueue* timers_ = nullptr;
    runtime::Clock* clock_ = nullptr;

    std::vector<Slot> slots_;
    std::size_t records_ = 0;
    // Tickets found cancelled by `tick()`; a member so a steady-state tick allocates nothing.
    std::vector<NetTicket> scratch_;
    NetDiagnostics diagnostics_;
    // Set by the destructor: nothing is resolved or delivered after it.
    bool stopped_ = false;
};

} // namespace tessera::appfw::detail
