// SPDX-License-Identifier: MIT

#include "fake_hal.hpp"
#include "tessera/harness/harness_handler.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/script_runner.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::harness::ActionCommand;
using tessera::harness::ArtifactSink;
using tessera::harness::AuditArtifact;
using tessera::harness::AutomationHost;
using tessera::harness::encode_script_report;
using tessera::harness::HarnessCommandHandler;
using tessera::harness::JsonValue;
using tessera::harness::parse_script;
using tessera::harness::ProtocolError;
using tessera::harness::ProtocolErrorCode;
using tessera::harness::Script;
using tessera::harness::ScriptAction;
using tessera::harness::ScriptBudget;
using tessera::harness::ScriptErrorCode;
using tessera::harness::ScriptRunner;
using tessera::harness::to_json;
using tessera::harness::TraceArtifact;
using tessera::harness::testing::ManualClock;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "script_runner: " << message << "\n";
        ++failures;
    }
    return condition;
}

class FakeHost final : public AutomationHost {
public:
    [[nodiscard]] std::expected<JsonValue, ProtocolError>
    canonical_snapshot(bool include_hidden) override {
        ++snapshot_calls;
        last_include_hidden = include_hidden;
        return snapshot;
    }

    [[nodiscard]] std::expected<void, ProtocolError>
    dispatch_action(const ActionCommand& action) override {
        actions.push_back(action);
        // A host that spends simulated time is how the runner's budget is exercised without a
        // wall clock: the clock only moves because this handler moves it.
        if (clock != nullptr && action_advance_us != 0) {
            clock->advance_us(action_advance_us);
        }
        if (action_error.has_value()) {
            return std::unexpected(*action_error);
        }
        return {};
    }

    [[nodiscard]] std::expected<TraceArtifact, ProtocolError> collect_trace() override {
        ++trace_calls;
        return trace;
    }

    [[nodiscard]] std::expected<AuditArtifact, ProtocolError>
    run_audit(std::string_view profile) override {
        audit_profiles.emplace_back(profile);
        return audit;
    }

    JsonValue snapshot = JsonValue::object(
        {{"nodes", JsonValue::array({JsonValue::object({{"text", JsonValue::string("hi")}})})}});
    TraceArtifact trace{"{\"traceEvents\":[]}", 2};
    AuditArtifact audit{"{\"score\":97}", 97.0, true};
    std::vector<ActionCommand> actions;
    std::vector<std::string> audit_profiles;
    std::optional<ProtocolError> action_error;
    ManualClock* clock = nullptr;
    std::uint64_t action_advance_us = 0;
    bool last_include_hidden = false;
    int snapshot_calls = 0;
    int trace_calls = 0;
};

class FakeSink final : public ArtifactSink {
public:
    [[nodiscard]] std::expected<void, ProtocolError> write(std::string_view path,
                                                           std::string_view bytes) override {
        writes.emplace_back(std::string(path), std::string(bytes));
        return {};
    }

    std::vector<std::pair<std::string, std::string>> writes;
};

struct Fixture {
    FakeHost host;
    FakeSink sink;
    ManualClock clock;
    HarnessCommandHandler handler{host, sink};
};

// Pumps until the runner stops, with a hard guard so a stuck runner fails the test instead of
// hanging it (the runner is bounded; the guard only protects the test itself).
std::size_t drain(ScriptRunner& runner) {
    std::size_t pumps = 0;
    while (runner.pump()) {
        ++pumps;
        if (pumps > 1000) {
            break;
        }
    }
    return pumps;
}

constexpr std::string_view kHappyScript = R"({
    "scriptVersion": 1,
    "profile": {"width": 1024, "height": 768, "scale": 2.0, "locale": "en-GB"},
    "app": {"config": "tessera.json"},
    "steps": [
        {"action": "click", "uiId": "42"},
        {"action": "type", "uiId": "7", "text": "alice", "itemKey": "row-1"},
        {"action": "focus", "uiId": "42"},
        {"action": "scroll", "uiId": "5", "dx": 0, "dy": 120.5},
        {"action": "snapshot", "includeHidden": true, "path": "out/snapshot.json"},
        {"action": "trace", "path": "out/trace.json"},
        {"action": "audit", "profile": "desktop"}
    ]
})";

} // namespace

int main() {
    // Happy path: seven steps run in order through the real handler seam, artifacts land, report
    // carries the section 5 fields.
    {
        Fixture fixture;
        const auto parsed = parse_script(kHappyScript);
        expect(parsed.has_value(), "happy script parses");
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock);
        drain(runner);

        const auto& report = runner.report();
        expect(report.passed && !report.cancelled && !report.error.has_value(), "run passes");
        expect(report.steps_total == 7 && report.steps_executed == 7, "seven steps reported");
        expect(report.steps.size() == 7 && runner.finished(), "runner finished");
        expect(report.elapsed_us == 0, "no clock movement means zero simulated duration");
        expect(fixture.host.actions.size() == 4, "four actions dispatched");
        expect(fixture.host.actions[0].kind == tessera::harness::ActionKind::Click &&
                   fixture.host.actions[0].ui_id == 42,
               "click target forwarded");
        expect(fixture.host.actions[1].kind == tessera::harness::ActionKind::SetValue &&
                   fixture.host.actions[1].value == "alice",
               "type becomes setValue");
        expect(fixture.host.snapshot_calls == 1 && fixture.host.last_include_hidden,
               "snapshot includeHidden forwarded");
        expect(fixture.host.trace_calls == 1, "trace collected");
        expect(fixture.host.audit_profiles == std::vector<std::string>{"desktop"},
               "audit profile forwarded");
        expect(fixture.sink.writes.size() == 3, "snapshot/trace/audit artifacts written");
        expect(fixture.sink.writes[0].first == "out/snapshot.json", "snapshot path");
        expect(fixture.sink.writes[1].first == "out/trace.json", "trace path");
        expect(fixture.sink.writes[2].first == "out/audit.json", "audit default path");
        expect(report.steps[4].artifact_path == "out/snapshot.json", "snapshot artifact echoed");
        expect(report.steps[4].ok && report.steps[4].result.find("snapshot") != nullptr,
               "snapshot result embedded");
        expect(report.steps[5].artifact_path == "out/trace.json", "trace artifact echoed");
        expect(report.steps[6].artifact_path == "out/audit.json", "audit artifact echoed");
        expect(report.steps[0].ui_id == 42 && report.steps[0].action == ScriptAction::Click,
               "step target/action reported");

        const auto json = to_json(encode_script_report(report));
        expect(json.has_value(), "report encodes");
        expect(json->find("\"passed\":true") != std::string::npos, "report passed field");
        expect(json->find("\"action\":\"type\"") != std::string::npos, "report action field");
        expect(json->find("\"uiId\":\"7\"") != std::string::npos, "report target id field");
        expect(json->find("\"artifact\":\"out/snapshot.json\"") != std::string::npos,
               "report artifact field");
        expect(json->find("\"stepsRun\":7") != std::string::npos, "report step count");
    }

    // A snapshot with no path is embedded in the step result instead of written.
    {
        Fixture fixture;
        const auto parsed = parse_script(R"({
            "scriptVersion":1,
            "steps":[{"action":"snapshot","includeHidden":false}]
        })");
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock);
        drain(runner);
        expect(runner.report().passed, "embedded snapshot run passes");
        expect(fixture.sink.writes.empty(), "no artifact without a path");
        expect(runner.report().steps[0].result.find("snapshot") != nullptr,
               "snapshot embedded in result");
    }

    // The step-count cap stops the run before the extra handler calls and reports the code.
    {
        Fixture fixture;
        const auto parsed = parse_script(R"({
            "scriptVersion":1,
            "steps":[{"action":"click","uiId":"1"},
                     {"action":"click","uiId":"2"},
                     {"action":"click","uiId":"3"}]
        })");
        ScriptBudget budget;
        budget.max_steps = 2;
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock, budget);
        drain(runner);
        const auto& report = runner.report();
        expect(!report.passed, "capped run does not pass");
        expect(report.error.has_value() && report.error->code == ScriptErrorCode::StepCapExceeded,
               "capped run reports stepCapExceeded");
        expect(report.steps_executed == 2 && report.steps_total == 3, "two of three steps ran");
        expect(fixture.host.actions.size() == 2, "third handler call never happened");
        expect(runner.finished(), "capped runner finished");
    }

    // A per-step budget overrun is detected from the injected clock, never a wall clock.
    {
        Fixture fixture;
        fixture.host.clock = &fixture.clock;
        fixture.host.action_advance_us = 5000;
        const auto parsed = parse_script(R"({
            "scriptVersion":1,
            "steps":[{"action":"click","uiId":"1"},{"action":"click","uiId":"2"}]
        })");
        ScriptBudget budget;
        budget.step_timeout_us = 1000;
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock, budget);
        drain(runner);
        const auto& report = runner.report();
        expect(!report.passed && report.steps.size() == 1, "overrun stops the run");
        expect(!report.steps[0].ok && report.steps[0].error.code == ScriptErrorCode::StepTimeout,
               "overrun reports stepTimeout");
        expect(report.steps[0].error.protocol_code == ProtocolErrorCode::Timeout,
               "overrun carries the protocol timeout code");
        expect(report.steps[0].elapsed_us == 5000, "overrun duration recorded");
        expect(fixture.host.actions.size() == 1, "second step never dispatched");
    }

    // A zero per-step budget times out before dispatch: no handler call, no fabricated result.
    {
        Fixture fixture;
        const auto parsed =
            parse_script(R"({"scriptVersion":1,"steps":[{"action":"click","uiId":"1"}]})");
        ScriptBudget budget;
        budget.step_timeout_us = 0;
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock, budget);
        drain(runner);
        expect(!runner.report().passed, "zero budget fails");
        expect(fixture.host.actions.empty(), "zero budget never dispatches");
        expect(runner.report().steps[0].error.code == ScriptErrorCode::StepTimeout,
               "zero budget reports stepTimeout");
    }

    // The whole-run budget is checked between steps and reports its own code.
    {
        Fixture fixture;
        const auto parsed = parse_script(R"({
            "scriptVersion":1,
            "steps":[{"action":"click","uiId":"1"},{"action":"click","uiId":"2"}]
        })");
        ScriptBudget budget;
        budget.run_timeout_us = 1000;
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock, budget);
        expect(runner.pump(), "first step runs");
        fixture.clock.advance_us(2000);
        drain(runner);
        const auto& report = runner.report();
        expect(!report.passed && report.error.has_value() &&
                   report.error->code == ScriptErrorCode::RunTimeout,
               "runTimeout reported");
        expect(report.steps_executed == 1 && fixture.host.actions.size() == 1,
               "second step never ran");
    }

    // A handler failure keeps its protocol code and stops the run.
    {
        Fixture fixture;
        fixture.host.action_error =
            ProtocolError{ProtocolErrorCode::UiIdNotFound, "no such node", "42", std::nullopt};
        const auto parsed = parse_script(R"({
            "scriptVersion":1,
            "steps":[{"action":"click","uiId":"42"},{"action":"click","uiId":"43"}]
        })");
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock);
        drain(runner);
        const auto& report = runner.report();
        expect(!report.passed && report.steps.size() == 1, "handler failure stops the run");
        expect(report.steps[0].error.code == ScriptErrorCode::HandlerError,
               "handler failure reports handlerError");
        expect(report.steps[0].error.protocol_code == ProtocolErrorCode::UiIdNotFound,
               "protocol code preserved");
        expect(fixture.host.actions.size() == 1, "later steps not dispatched");
    }

    // A malformed step is rejected at parse time, so the runner never dispatches anything.
    {
        Fixture fixture;
        const auto parsed = parse_script(R"({"scriptVersion":1,"steps":[{"action":"click"}]})");
        expect(!parsed.has_value(), "malformed step rejected");
        expect(parsed.error().code == ScriptErrorCode::InvalidStep && parsed.error().step == 0,
               "invalidStep with the step index");
        expect(fixture.host.actions.empty(), "no handler call for a rejected script");
    }

    // Cancellation before the first step and between steps both stop without dispatching further.
    {
        Fixture fixture;
        const auto parsed = parse_script(R"({
            "scriptVersion":1,
            "steps":[{"action":"click","uiId":"1"},{"action":"click","uiId":"2"}]
        })");
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock);
        runner.cancel();
        expect(!runner.pump(), "cancelled pump stops");
        expect(runner.report().cancelled && !runner.report().passed, "report marked cancelled");
        expect(runner.report().error.has_value() &&
                   runner.report().error->code == ScriptErrorCode::Cancelled,
               "cancelled report code");
        expect(fixture.host.actions.empty(), "cancelled run dispatches nothing");
    }
    {
        Fixture fixture;
        const auto parsed = parse_script(R"({
            "scriptVersion":1,
            "steps":[{"action":"click","uiId":"1"},{"action":"click","uiId":"2"}]
        })");
        ScriptRunner runner(*parsed, fixture.handler, fixture.sink, fixture.clock);
        expect(runner.pump(), "first step runs before cancellation");
        runner.cancel();
        expect(!runner.pump(), "second pump observes cancellation");
        expect(fixture.host.actions.size() == 1, "second step never runs");
        expect(runner.report().steps_executed == 1, "one step recorded");
    }

    if (failures != 0) {
        std::cerr << "script_runner: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
