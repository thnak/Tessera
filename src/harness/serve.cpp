// SPDX-License-Identifier: MIT

#include "tessera/harness/serve.hpp"

#include <thread>

namespace tessera::harness {

ServeResult serve(LineTransport& transport, CommandHandler& handler, SessionClock& clock,
                  const ServeOptions& options) {
    SessionExecutor session(transport, handler, clock, options.limits);
    ServeResult result;
    const auto absorb = [&result](const SessionPumpStats& stats) {
        result.commands_dispatched += stats.commands_dispatched;
        result.responses_written += stats.responses_written;
    };

    while (!session.closed()) {
        if (options.stop != nullptr && options.stop->load(std::memory_order_acquire)) {
            absorb(session.shutdown());
            result.end = ServeEnd::Stopped;
            return result;
        }
        ++result.iterations;
        if (options.on_tick) {
            options.on_tick();
        }
        const SessionPumpStats stats = session.pump();
        absorb(stats);
        if (session.closed()) {
            break;
        }
        if (stats.output_blocked) {
            std::this_thread::sleep_for(options.output_blocked_sleep);
        } else if (stats.lines_read == 0 && stats.commands_dispatched == 0 &&
                   session.queued_requests() == 0) {
            // pump() reports has_more_work for any open input, so idleness is judged from the
            // work this slice actually did.
            (void)transport.wait_readable(options.wait_slice);
        }
    }
    return result;
}

} // namespace tessera::harness
