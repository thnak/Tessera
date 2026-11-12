// SPDX-License-Identifier: MIT
//
// The stdio serve loop: it waits on the transport's bounded readiness seam instead of spinning,
// answers requests in order, ends on EOF or on the stop flag, and creates no thread. Everything
// runs against a scripted transport, so no handle, wall clock, or sleep decides the outcome.

#include "session_test_support.hpp"
#include "tessera/harness/serve.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace st = session_test;
using tessera::harness::serve;
using tessera::harness::ServeEnd;
using tessera::harness::ServeOptions;

namespace {

// Idle until `idle_waits` waits have happened, then delivers its lines and EOF. It counts waits
// and records their timeouts; an optional hook runs inside each wait (e.g. to set a stop flag).
class WaitingTransport final : public st::LineTransport {
public:
    [[nodiscard]] std::expected<std::string, st::TransportError> read_line() override {
        if (waits_done >= idle_waits && next < inbound.size()) {
            return inbound[next++];
        }
        if (waits_done >= idle_waits && eof_after_lines) {
            return std::unexpected(st::TransportError{st::TransportErrorCode::Closed, "eof"});
        }
        ++idle_reads;
        return std::unexpected(st::TransportError{st::TransportErrorCode::WouldBlock, "idle"});
    }
    [[nodiscard]] std::expected<void, st::TransportError>
    write_line(std::string_view line) override {
        outbound.emplace_back(line);
        return {};
    }
    [[nodiscard]] bool wait_readable(std::chrono::milliseconds timeout) override {
        ++waits_done;
        timeouts.push_back(timeout);
        if (on_wait) {
            on_wait();
        }
        return waits_done >= idle_waits;
    }
    [[nodiscard]] std::size_t max_line_bytes() const noexcept override {
        return 4096;
    }
    [[nodiscard]] bool closed() const noexcept override {
        return closed_state;
    }
    void close() noexcept override {
        closed_state = true;
    }

    std::vector<std::string> inbound;
    std::size_t next = 0;
    std::size_t idle_waits = 0;
    bool eof_after_lines = true;
    std::size_t waits_done = 0;
    std::size_t idle_reads = 0;
    std::vector<std::chrono::milliseconds> timeouts;
    std::vector<std::string> outbound;
    std::function<void()> on_wait;
    bool closed_state = false;
};

} // namespace

int main() {
    // A silent peer costs one bounded wait per iteration, not a spin; the request that arrives
    // afterwards is answered and EOF ends the loop.
    {
        WaitingTransport transport;
        transport.idle_waits = 3;
        transport.inbound = {st::request_line(1, st::SnapshotCommand{false})};
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        ServeOptions options;
        options.wait_slice = std::chrono::milliseconds(25);
        const auto result = serve(transport, handler, clock, options);
        st::expect(result.end == ServeEnd::InputClosed, "EOF ends the loop");
        st::expect(result.commands_dispatched == 1 && handler.seen.size() == 1,
                   "the late request was dispatched");
        st::expect(transport.outbound.size() == 1, "and answered");
        st::expect(transport.waits_done == 3, "one wait per idle iteration");
        st::expect(transport.idle_reads == 3, "exactly one idle read per wait: no spinning");
        bool all_bounded = true;
        for (const auto timeout : transport.timeouts) {
            all_bounded = all_bounded && timeout == std::chrono::milliseconds(25);
        }
        st::expect(all_bounded, "every wait used the configured bounded slice");
    }

    // Requests keep their order and a burst needs no waits while work is available.
    {
        WaitingTransport transport;
        transport.inbound = {st::request_line(1, st::SnapshotCommand{false}),
                             st::request_line(2, st::SnapshotCommand{true}),
                             st::request_line(3, st::SnapshotCommand{false})};
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        const auto result = serve(transport, handler, clock);
        st::expect(result.responses_written == 3, "three responses written");
        st::expect(transport.waits_done == 0, "no wait while input is immediately available");
        st::expect(transport.outbound.size() == 3, "three frames out");
        for (std::size_t index = 0; index < transport.outbound.size(); ++index) {
            const auto response = tessera::harness::decode_response(transport.outbound[index]);
            st::expect(response.has_value() && response->id == index + 1, "responses in order");
        }
    }

    // The stop flag interrupts an idle wait loop within one slice and cancels via shutdown.
    {
        WaitingTransport transport;
        transport.idle_waits = 1000; // never becomes readable on its own
        transport.eof_after_lines = false;
        std::atomic<bool> stop{false};
        transport.on_wait = [&] {
            if (transport.waits_done == 4) {
                stop.store(true, std::memory_order_release);
            }
        };
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        ServeOptions options;
        options.stop = &stop;
        const auto result = serve(transport, handler, clock, options);
        st::expect(result.end == ServeEnd::Stopped, "stop flag ends the loop");
        st::expect(transport.waits_done == 4, "stop was observed on the next iteration");
        st::expect(transport.closed(), "shutdown closed the transport");
    }

    // A stop flag that is already set serves nothing.
    {
        WaitingTransport transport;
        transport.inbound = {st::request_line(1, st::SnapshotCommand{false})};
        std::atomic<bool> stop{true};
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        ServeOptions options;
        options.stop = &stop;
        const auto result = serve(transport, handler, clock, options);
        st::expect(result.end == ServeEnd::Stopped && result.iterations == 0,
                   "pre-set stop returns before the first pump");
        st::expect(handler.seen.empty(), "no command ran");
    }

    // on_tick runs once per iteration.
    {
        WaitingTransport transport;
        transport.idle_waits = 2;
        st::ManualSessionClock clock;
        st::RecordingHandler handler;
        std::size_t ticks = 0;
        ServeOptions options;
        options.on_tick = [&ticks] { ++ticks; };
        const auto result = serve(transport, handler, clock, options);
        st::expect(ticks == result.iterations && ticks >= 3, "tick hook ran every iteration");
    }

    if (st::failures != 0) {
        std::cerr << "harness_serve: " << st::failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
