// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/command_handler.hpp"
#include "tessera/harness/line_transport.hpp"
#include "tessera/harness/session.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>

namespace tessera::harness {

struct ServeOptions {
    // Longest single wait for input. This is also how often `stop` is observed while the peer is
    // silent, so it bounds shutdown latency; it must stay small.
    std::chrono::milliseconds wait_slice{50};
    // Sleep used instead of a readiness wait while the output side is blocked, so a stalled
    // reader cannot turn the loop into a busy spin.
    std::chrono::milliseconds output_blocked_sleep{1};
    // Set by another thread (a signal handler flag, a test) to end the loop. May be null.
    const std::atomic<bool>* stop = nullptr;
    // Runs once per loop iteration before the session is pumped, so a host can advance its
    // simulation by one tick. May be empty.
    std::function<void()> on_tick;
    SessionLimits limits;
};

enum class ServeEnd {
    InputClosed, // EOF or the peer went away; all queued work was answered
    Stopped,     // `stop` was observed; queued work was cancelled via SessionExecutor::shutdown()
};

struct ServeResult {
    ServeEnd end = ServeEnd::InputClosed;
    std::size_t iterations = 0;
    std::size_t commands_dispatched = 0;
    std::size_t responses_written = 0;
};

// Serves the line protocol until the input closes or `stop` is set. The loop is single-threaded
// and never spins: each iteration pumps one bounded slice, and when no work remains it waits on
// `transport.wait_readable(wait_slice)`. No thread is created, so there is nothing to detach or
// join. The caller owns the transport, handler, and clock.
[[nodiscard]] ServeResult serve(LineTransport& transport, CommandHandler& handler,
                                SessionClock& clock, const ServeOptions& options = {});

} // namespace tessera::harness
