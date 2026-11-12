// SPDX-License-Identifier: MIT
//
// End-to-end lane: one JSON script, the real ScriptRunner/HarnessCommandHandler, the concrete
// EngineAutomationHost and a real AppHost scene. Snapshot, click, type, focus, scroll, trace and
// audit all execute through the production input and diagnostics paths; the same script run twice
// produces byte-identical reports.

#include "engine_host_support.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/harness_handler.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/script_runner.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace harness = tessera::harness;
namespace automation = tessera::automation;
namespace testing = tessera::harness::testing;
namespace ui = tessera::ui;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "engine_host: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Deterministic runner clock: the lane must never read a wall clock.
class ZeroClock final : public harness::SessionClock {
public:
    [[nodiscard]] std::uint64_t now_us() const override {
        return 0;
    }
};

class MemorySink final : public harness::ArtifactSink {
public:
    [[nodiscard]] std::expected<void, harness::ProtocolError>
    write(std::string_view path, std::string_view bytes) override {
        writes.emplace_back(std::string(path), std::string(bytes));
        return {};
    }

    [[nodiscard]] const std::string* find(std::string_view path) const {
        for (const auto& write : writes) {
            if (write.first == path) {
                return &write.second;
            }
        }
        return nullptr;
    }

    std::vector<std::pair<std::string, std::string>> writes;
};

constexpr std::string_view kScript = R"({
    "scriptVersion": 1,
    "profile": {"width": 320, "height": 240, "scale": 1.0, "locale": "en-US"},
    "steps": [
        {"action": "snapshot"},
        {"action": "click", "uiId": "9003"},
        {"action": "type", "uiId": "9004", "text": "Ada"},
        {"action": "focus", "uiId": "9004"},
        {"action": "scroll", "uiId": "9005", "dx": 0, "dy": 40},
        {"action": "snapshot"},
        {"action": "trace"},
        {"action": "audit", "profile": "desktop"}
    ]
})";

struct Run {
    tessera::diagnostics::TraceRecorder recorder{256};
    tessera::diagnostics::MetricsRegistry metrics;
    MemorySink sink;
    harness::ScriptReport report;
    std::string report_json;
};

bool run_script(testing::HostFixture& scene, Run& run) {
    auto parsed = harness::parse_script(kScript);
    if (!expect(parsed.has_value(), "the script parses")) {
        return false;
    }
    automation::AutomationDiagnostics hooks;
    hooks.trace = &run.recorder;
    hooks.metrics = &run.metrics;
    hooks.enabled = true;
    scene.automation.set_diagnostics(hooks);

    harness::HarnessCommandHandler handler(scene.automation, run.sink);
    ZeroClock clock;
    harness::ScriptRunner runner(std::move(*parsed), handler, run.sink, clock);
    std::size_t pumps = 0;
    while (runner.pump()) {
        if (++pumps > harness::kMaxScriptSteps) {
            expect(false, "the runner is bounded");
            break;
        }
    }
    run.report = runner.report();
    auto json = harness::to_json(encode_script_report(run.report));
    if (!expect(json.has_value(), "the report encodes")) {
        return false;
    }
    run.report_json = std::move(*json);
    return true;
}

harness::JsonValue snapshot_of(const harness::ScriptStepReport& step) {
    const harness::JsonValue* snapshot = step.result.find("snapshot");
    return snapshot == nullptr ? harness::JsonValue{} : *snapshot;
}

// Reads one optional string field of a snapshot entry without risking a null dereference.
std::string_view field_of(const harness::JsonValue& node, std::string_view key) {
    const harness::JsonValue* value = node.find(key);
    if (value == nullptr) {
        return {};
    }
    const auto text = value->as_string();
    return text.has_value() ? *text : std::string_view{};
}

} // namespace

int main() {
    testing::HostFixture first;
    Run first_run;
    if (!run_script(first, first_run)) {
        return 1;
    }

    const harness::ScriptReport& report = first_run.report;
    expect(report.passed && !report.cancelled && !report.error.has_value(), "the run passes");
    expect(report.steps_total == 8 && report.steps_executed == 8, "eight steps ran");
    for (const harness::ScriptStepReport& step : report.steps) {
        expect(step.ok, "every step reports ok");
    }

    // Host counters: two snapshots, four actions, one trace, one audit, and one trace event per
    // command while diagnostics are enabled.
    const auto& stats = first.automation.stats();
    expect(stats.snapshots == 2, "two snapshots were built");
    expect(stats.actions == 4, "four actions were dispatched");
    expect(stats.traces == 1 && stats.audits == 1, "trace and audit were requested once");
    expect(stats.trace_events == 8, "one trace event per command");
    expect(stats.settle_frames >= 1, "a dispatched input settled a frame at least once");
    expect(stats.settle_frames <= stats.actions + stats.snapshots,
           "at most one settle frame per command");
    expect(stats.emitted_nodes == 10, "the fixture tree has ten nodes");

    // Step 0: the canonical snapshot over the live scene.
    const harness::JsonValue before = snapshot_of(report.steps[0]);
    const harness::JsonValue* nodes = before.find("nodes");
    expect(nodes != nullptr && nodes->as_array() != nullptr, "snapshot carries a node array");
    expect(nodes != nullptr && nodes->as_array() != nullptr && nodes->as_array()->size() == 10,
           "all ten nodes are emitted");
    const harness::JsonValue* version = before.find("snapshotVersion");
    expect(version != nullptr && version->as_integer() == 1, "snapshot version is 1");
    const harness::JsonValue* sequence = before.find("sequence");
    expect(sequence != nullptr && sequence->as_string() == "0", "first snapshot sequence is 0");
    const harness::JsonValue* button = testing::snapshot_node(before, "9003");
    expect(button != nullptr, "the button carries its automation id");
    expect(button != nullptr && field_of(*button, "kind") == "Button", "button kind");
    expect(button != nullptr && button->find("bounds") != nullptr &&
               button->find("bounds")->find("width") != nullptr &&
               button->find("bounds")->find("width")->as_number() > 0.0,
           "button bounds come from layout");

    // Real input path: click bumps the fixture counter, type reaches the handler, focus lands, the
    // wheel scrolls the list.
    expect(first.fixture.count() == 1, "the click handler ran once");
    expect(first.fixture.typed_text() == "Ada", "typed text reached the edit handler");
    const ui::Node* field = testing::find_node(first.root, automation::kFixtureFieldId);
    expect(first.automation.input().router().focus_manager().focused() == field,
           "focus is on the field");
    expect(field != nullptr && field->paint_data().has_value() &&
               field->paint_data()->text == "Ada",
           "the field paints the typed value");
    const ui::Node* list = testing::find_node(first.root, automation::kFixtureListId);
    expect(list != nullptr && list->scroll_y() == 40.0f, "the wheel scrolled the list by 40px");

    // Step 5: the second snapshot observes applied state.
    const harness::JsonValue after = snapshot_of(report.steps[5]);
    const harness::JsonValue* label = testing::snapshot_node(after, "9002");
    expect(label != nullptr && field_of(*label, "text") == "Count: 1",
           "the counter label was repainted by the click handler");
    const harness::JsonValue* field_node = testing::snapshot_node(after, "9004");
    expect(field_node != nullptr && field_of(*field_node, "text") == "Ada",
           "the field shows the typed value");
    expect(field_node != nullptr && field_node->find("focused") != nullptr &&
               field_node->find("focused")->as_bool() == true,
           "the field is focused");
    const harness::JsonValue* sequence_after = after.find("sequence");
    expect(sequence_after != nullptr && sequence_after->as_string() == "1",
           "the snapshot sequence advanced");

    // Trace and audit wrote their documented artifacts.
    const std::string* trace = first_run.sink.find("out/trace.json");
    expect(trace != nullptr && trace->front() == '[', "the trace is a Chrome trace array");
    expect(trace != nullptr && trace->find("automation.click") != std::string::npos,
           "the trace carries the input span");
    const harness::JsonValue* span_count = report.steps[6].result.find("spanCount");
    expect(span_count != nullptr && span_count->as_integer() > 0, "the trace reports its spans");

    const std::string* audit = first_run.sink.find("out/audit.json");
    expect(audit != nullptr && audit->find("\"profile\":\"desktop\"") != std::string::npos,
           "the audit report names its profile");
    expect(audit != nullptr && audit->find("\"passed\":true") != std::string::npos,
           "the audit passes on a clean run");
    const harness::JsonValue* score = report.steps[7].result.find("score");
    expect(score != nullptr && score->as_number() == 100.0, "the audit score is 100");

    // Determinism: a second host and a second run produce the same report bytes.
    testing::HostFixture second;
    Run second_run;
    if (!run_script(second, second_run)) {
        return 2;
    }
    expect(second_run.report_json == first_run.report_json,
           "the same script produces byte-identical reports");
    expect(second.fixture.count() == first.fixture.count(), "side effects are identical");

    if (failures != 0) {
        std::cerr << "engine_host: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "engine_host: ok\n";
    return 0;
}
