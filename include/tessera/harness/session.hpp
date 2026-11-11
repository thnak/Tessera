// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/command_handler.hpp"
#include "tessera/harness/line_transport.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace tessera::harness {

// Time source for the executor's per-command budget. Structural/golden runs must use simulation
// time only (spec section 7), so a host injects the runtime's manual clock; audit mode and the CLI
// use the monotonic implementation below.
class SessionClock {
public:
    SessionClock() = default;
    SessionClock(const SessionClock&) = delete;
    SessionClock& operator=(const SessionClock&) = delete;
    virtual ~SessionClock() = default;

    [[nodiscard]] virtual std::uint64_t now_us() const = 0;
};

class SteadySessionClock final : public SessionClock {
public:
    [[nodiscard]] std::uint64_t now_us() const override;
};

// One pump is one bounded slice of work, sized for a RuntimeLoop tick: at most this many input
// lines are decoded and this many handlers are called before control returns. A queue at its cap
// stops reading instead of growing without bound or dropping a request, so back-pressure reaches
// the transport and a slow UI cannot make the harness allocate without limit.
struct SessionLimits {
    std::size_t max_reads_per_pump = 16;
    std::size_t max_commands_per_pump = 8;
    std::size_t max_queued_requests = 64;
    std::uint64_t command_timeout_us = 5'000'000;
};

// Narrows one pump to the work a single `RuntimeLoop` tick may do. A host that drives the session
// from a fixed-step loop passes the same bound it gives a tick (its
// `FixedStepConfig::max_updates_per_frame` and the reads that can feed them), so one pump can never
// outrun one tick. Zero means "use the configured SessionLimits value"; a non-zero value is clamped
// to it and is never raised, so a budget can only narrow work.
struct SessionTickBudget {
    std::size_t max_reads = 0;
    std::size_t max_commands = 0;

    friend constexpr bool operator==(const SessionTickBudget&, const SessionTickBudget&) = default;
};

struct SessionPumpStats {
    std::size_t lines_read = 0;
    std::size_t commands_dispatched = 0; // handler calls
    std::size_t requests_answered = 0;   // responses encoded (success or failure)
    std::size_t responses_written = 0;
    std::size_t dropped_frames = 0;    // framing rejection with no id to answer
    std::size_t dropped_responses = 0; // response could not be encoded or the session ended
    bool read_backpressure = false;    // the request queue is full
    bool input_closed = false;
    bool output_blocked = false;
    bool closed = false;
    bool has_more_work = false;
};

// Single-threaded request loop: read -> decode -> dispatch -> encode -> write, in that order, from
// one FIFO. Responses therefore always leave in request order, no detached task exists, and a pump
// never waits on I/O, a handler, or another thread.
//
// The command timeout is enforced at two points without interrupting a running handler: a queued
// request whose deadline passed before dispatch is answered with Timeout, and a handler whose
// simulated elapsed time exceeds the budget has its result replaced with Timeout. A synchronous
// handler cannot be preempted, which is exactly why handlers are serialized and bounded.
class SessionExecutor {
public:
    SessionExecutor(LineTransport& transport, CommandHandler& handler, SessionClock& clock,
                    SessionLimits limits = {});
    ~SessionExecutor() = default;

    SessionExecutor(const SessionExecutor&) = delete;
    SessionExecutor& operator=(const SessionExecutor&) = delete;

    // One bounded slice. The default budget is the configured SessionLimits; a tick budget only
    // narrows it, so a caller can size one pump to one runtime tick.
    [[nodiscard]] SessionPumpStats pump(SessionTickBudget budget = {});
    // Cancels queued requests in arrival order (each is answered with an Internal diagnostic),
    // writes as much as the transport accepts without waiting, then closes. Bytes a peer refused
    // to read are dropped: shutdown never blocks.
    [[nodiscard]] SessionPumpStats shutdown();

    [[nodiscard]] bool closed() const noexcept {
        return finished_;
    }
    [[nodiscard]] std::size_t queued_requests() const noexcept {
        return queue_.size();
    }
    [[nodiscard]] const SessionLimits& limits() const noexcept {
        return limits_;
    }

private:
    struct Pending {
        RequestId id = 0;
        std::optional<CommandPayload> command; // nullopt: the request itself failed to decode
        ProtocolError error;
        std::uint64_t deadline_us = 0;
    };

    [[nodiscard]] std::size_t read_budget(SessionTickBudget budget) const noexcept;
    [[nodiscard]] std::size_t command_budget(SessionTickBudget budget) const noexcept;

    void read_phase(SessionPumpStats& stats, std::size_t max_reads);
    void dispatch_phase(SessionPumpStats& stats, std::size_t max_commands);
    void flush_output(SessionPumpStats& stats);
    void handle_line(std::string_view line);
    void answer(const Pending& pending, SessionPumpStats& stats);
    void answer_error(RequestId id, ProtocolError error, SessionPumpStats& stats);
    void enqueue_response(Response response, SessionPumpStats& stats);
    void finish_if_done();
    [[nodiscard]] std::uint64_t deadline_from_now() const noexcept;

    LineTransport& transport_;
    CommandHandler& handler_;
    SessionClock& clock_;
    SessionLimits limits_;
    std::deque<Pending> queue_;
    std::deque<std::string> pending_out_;
    bool input_closed_ = false;
    bool transport_closed_ = false;
    bool output_blocked_ = false;
    bool finished_ = false;
};

} // namespace tessera::harness
