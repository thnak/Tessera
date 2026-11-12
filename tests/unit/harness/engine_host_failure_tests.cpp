// SPDX-License-Identifier: MIT
//
// Failure paths through the wired lane: an action v1 cannot execute, an unknown target, the step
// cap, and both timeout rules. Every case runs the real ScriptRunner over the real
// EngineAutomationHost, so the codes are the ones a client actually sees.

#include "engine_host_support.hpp"
#include "tessera/harness/harness_handler.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/script_runner.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace harness = tessera::harness;
namespace automation = tessera::automation;
namespace testing = tessera::harness::testing;
namespace ui = tessera::ui;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "engine_host_failure: " << message << "\n";
        ++failures;
    }
    return condition;
}

class SteppableClock final : public harness::SessionClock {
public:
    [[nodiscard]] std::uint64_t now_us() const override {
        return now_us_;
    }
    void advance_us(std::uint64_t micros) {
        now_us_ += micros;
    }

private:
    std::uint64_t now_us_ = 0;
};

class MemorySink final : public harness::ArtifactSink {
public:
    [[nodiscard]] std::expected<void, harness::ProtocolError>
    write(std::string_view path, std::string_view bytes) override {
        last_path = path;
        last_bytes = bytes;
        return {};
    }

    std::string last_path;
    std::string last_bytes;
};

struct Lane {
    testing::HostFixture scene;
    MemorySink sink;
    SteppableClock clock;
    harness::HarnessCommandHandler handler{scene.automation, sink};
};

std::size_t drain(harness::ScriptRunner& runner, std::size_t guard = 512) {
    std::size_t pumps = 0;
    while (runner.pump() && pumps++ < guard) {
    }
    return pumps;
}

harness::ScriptBudget budget_with(std::size_t max_steps, std::uint64_t step_timeout_us,
                                  std::uint64_t run_timeout_us) {
    harness::ScriptBudget budget;
    budget.max_steps = max_steps;
    budget.step_timeout_us = step_timeout_us;
    budget.run_timeout_us = run_timeout_us;
    return budget;
}

} // namespace

int main() {
    // Actions the v1 host cannot execute are rejected at parse time, before any host call: a spec
    // action becomes unsupportedAction, a typo stays unknownAction.
    {
        Lane lane;
        const auto unsupported = harness::parse_script(R"({
            "scriptVersion": 1,
            "steps": [{"action": "screenshot", "path": "out/a.png"}]
        })");
        expect(!unsupported.has_value(), "a spec-only action is rejected");
        expect(unsupported.error().code == harness::ScriptErrorCode::UnsupportedAction,
               "a spec-only action reports unsupportedAction");
        expect(unsupported.error().step == 0, "the rejected step index is reported");

        const auto unknown =
            harness::parse_script(R"({"scriptVersion":1,"steps":[{"action":"frobnicate"}]})");
        expect(!unknown.has_value() &&
                   unknown.error().code == harness::ScriptErrorCode::UnknownAction,
               "a typo reports unknownAction");
        expect(lane.scene.automation.stats().actions == 0, "no host action was dispatched");
    }

    // An unknown target fails the step with the protocol code, and stops the run.
    {
        Lane lane;
        const auto parsed = harness::parse_script(R"({
            "scriptVersion": 1,
            "steps": [
                {"action": "click", "uiId": "424242"},
                {"action": "click", "uiId": "9003"}
            ]
        })");
        expect(parsed.has_value(), "the script parses");
        harness::ScriptRunner runner(*parsed, lane.handler, lane.sink, lane.clock);
        drain(runner);
        const auto& report = runner.report();
        expect(!report.passed && report.steps.size() == 1, "the run stops on the bad target");
        expect(report.steps[0].error.code == harness::ScriptErrorCode::HandlerError,
               "a host failure reports handlerError");
        expect(report.steps[0].error.protocol_code == harness::ProtocolErrorCode::UiIdNotFound,
               "the host's uiIdNotFound code is preserved");
        expect(lane.scene.fixture.count() == 0, "the later step never ran");
    }

    // The step cap stops the run before the extra dispatch and reports its own code.
    {
        Lane lane;
        const auto parsed = harness::parse_script(R"({
            "scriptVersion": 1,
            "steps": [{"action": "click", "uiId": "9003"}, {"action": "click", "uiId": "9003"}]
        })");
        harness::ScriptRunner runner(*parsed, lane.handler, lane.sink, lane.clock,
                                     budget_with(1, 5'000'000, 0));
        drain(runner);
        const auto& report = runner.report();
        expect(!report.passed && report.error.has_value() &&
                   report.error->code == harness::ScriptErrorCode::StepCapExceeded,
               "the step cap reports stepCapExceeded");
        expect(report.steps_executed == 1 && lane.scene.fixture.count() == 1,
               "only the first step dispatched");
    }

    // Whole-run budget: the clock is the only source of time, and the runner notices between steps.
    {
        Lane lane;
        const auto parsed = harness::parse_script(R"({
            "scriptVersion": 1,
            "steps": [{"action": "click", "uiId": "9003"}, {"action": "click", "uiId": "9003"}]
        })");
        harness::ScriptRunner runner(*parsed, lane.handler, lane.sink, lane.clock,
                                     budget_with(harness::kMaxScriptSteps, 5'000'000, 1000));
        expect(runner.pump(), "the first step runs");
        lane.clock.advance_us(2000);
        drain(runner);
        const auto& report = runner.report();
        expect(!report.passed && report.error.has_value() &&
                   report.error->code == harness::ScriptErrorCode::RunTimeout,
               "the run budget reports runTimeout");
        expect(lane.scene.fixture.count() == 1, "the second step never dispatched");
    }

    // A zero per-step budget expires before dispatch: the host is never called.
    {
        Lane lane;
        const auto parsed = harness::parse_script(
            R"({"scriptVersion":1,"steps":[{"action":"click","uiId":"9003"}]})");
        harness::ScriptRunner runner(*parsed, lane.handler, lane.sink, lane.clock,
                                     budget_with(harness::kMaxScriptSteps, 0, 0));
        drain(runner);
        const auto& report = runner.report();
        expect(!report.passed && report.steps.size() == 1 &&
                   report.steps[0].error.code == harness::ScriptErrorCode::StepTimeout,
               "an expired step budget reports stepTimeout");
        expect(lane.scene.automation.stats().actions == 0, "no handler call for an expired step");
    }

    // Host-side argument failures: a focus target that cannot take focus, and an itemKey the scene
    // graph cannot resolve.
    {
        Lane lane;
        harness::ActionCommand incompatible;
        incompatible.kind = harness::ActionKind::Focus;
        incompatible.ui_id = automation::kFixtureCounterId;
        const auto focus_result = lane.scene.automation.dispatch_action(incompatible);
        expect(!focus_result.has_value() &&
                   focus_result.error().code == harness::ProtocolErrorCode::InvalidArgument,
               "focusing a non-focusable node is invalidArgument");

        harness::ActionCommand keyed;
        keyed.kind = harness::ActionKind::Click;
        keyed.ui_id = automation::kFixtureButtonId;
        keyed.item_key = std::string("row-1");
        const auto keyed_result = lane.scene.automation.dispatch_action(keyed);
        expect(!keyed_result.has_value() &&
                   keyed_result.error().code == harness::ProtocolErrorCode::InvalidArgument,
               "itemKey targeting is rejected, not silently ignored");
        expect(lane.scene.fixture.count() == 0, "neither rejected action reached the scene");
    }

    // Hidden nodes are not targetable: the snapshot omits them, so a click cannot find them.
    {
        Lane lane;
        ui::Node* label = testing::find_node(lane.scene.root, automation::kFixtureCounterId);
        expect(label != nullptr, "the counter label exists");
        if (label != nullptr) {
            label->set_visible(false);
            harness::ActionCommand click;
            click.kind = harness::ActionKind::Click;
            click.ui_id = automation::kFixtureCounterId;
            const auto result = lane.scene.automation.dispatch_action(click);
            expect(!result.has_value() &&
                       result.error().code == harness::ProtocolErrorCode::UiIdNotFound,
                   "a hidden target is uiIdNotFound");
            label->set_visible(true);
        }
    }

    if (failures != 0) {
        std::cerr << "engine_host_failure: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "engine_host_failure: ok\n";
    return 0;
}
