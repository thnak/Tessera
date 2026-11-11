// SPDX-License-Identifier: MIT
//
// `SessionExecutor::pump(SessionTickBudget)` narrows one pump to one runtime tick: a fixed-step
// host passes the same bound it gives the loop, and the pump can then never read or dispatch more
// than that tick allows, even with a large configured SessionLimits.

#include "tessera/harness/in_process_transport.hpp"
#include "tessera/harness/session.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

namespace harness = tessera::harness;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "tick_budget: " << message << "\n";
        ++failures;
    }
    return condition;
}

class ZeroClock final : public harness::SessionClock {
public:
    [[nodiscard]] std::uint64_t now_us() const override {
        return 0;
    }
};

// Answers every command with a valid action result; the test only counts calls.
class CountingHandler final : public harness::CommandHandler {
public:
    [[nodiscard]] std::expected<harness::CommandResult, harness::ProtocolError>
    handle(const harness::CommandPayload& /*command*/) override {
        ++calls;
        return harness::CommandResult{
            harness::ActionResult{harness::ActionKind::Click, 1, std::nullopt}};
    }

    std::size_t calls = 0;
};

constexpr std::string_view kRequest = R"({"protocolVersion":1,"id":1,"cmd":"click","uiId":"1"})";

} // namespace

int main() {
    auto pair = harness::make_in_process_pair();
    // The peer writes four requests; the session endpoint reads them.
    for (int index = 0; index < 4; ++index) {
        const auto written = pair.second->write_line(kRequest);
        expect(written.has_value(), "the request was written");
    }

    CountingHandler handler;
    ZeroClock clock;
    harness::SessionLimits limits;
    limits.max_reads_per_pump = 16;
    limits.max_commands_per_pump = 16;
    harness::SessionExecutor executor(*pair.first, handler, clock, limits);

    // A tick budget of one command and two reads narrows the configured 16/16.
    harness::SessionTickBudget budget;
    budget.max_commands = 1;
    budget.max_reads = 2;
    const harness::SessionPumpStats first = executor.pump(budget);
    expect(first.lines_read == 2, "one pump reads only the tick's reads");
    expect(first.commands_dispatched == 1, "one pump dispatches only one command");
    expect(handler.calls == 1, "the handler ran once");
    expect(executor.queued_requests() == 1, "the unread line stays queued");

    const harness::SessionPumpStats second = executor.pump(budget);
    expect(handler.calls == 2, "the next tick dispatches the next command");
    expect(second.lines_read == 2 && second.commands_dispatched == 1,
           "the second tick stays bounded");

    // A default budget uses the configured limits and drains the rest in one pump.
    const harness::SessionPumpStats drained = executor.pump();
    expect(handler.calls == 4, "the default budget drains the queue");
    expect(drained.commands_dispatched == 2 && executor.queued_requests() == 0,
           "two commands were left and both dispatched");

    // A zeroed budget means "use SessionLimits", never "do nothing".
    expect(budget != harness::SessionTickBudget{}, "the narrowed budget is distinct");
    const harness::SessionTickBudget implicit{};
    expect(implicit.max_reads == 0 && implicit.max_commands == 0, "default budget is zeroed");

    // The runtime loop's fixed-step bound is what a host would pass: prove the two agree on shape.
    tessera::runtime::ManualClock loop_clock;
    tessera::runtime::RuntimeLoop loop(loop_clock);
    const std::uint32_t updates = loop.fixed_step().config().max_updates_per_frame;
    expect(updates > 0, "the runtime loop ticks at least one fixed update");

    if (failures != 0) {
        std::cerr << "tick_budget: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "tick_budget: ok\n";
    return 0;
}
