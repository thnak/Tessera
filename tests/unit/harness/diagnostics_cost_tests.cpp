// SPDX-License-Identifier: MIT
//
// Diagnostics are opt-in and cost nothing while disabled (spec sections 6.4 and 7): with the seams
// attached but `enabled == false`, a full script leaves the trace recorder empty, the metrics
// registry empty, and the host's diagnostics counters at zero. The same script produces the same
// report bytes as a host with no seams attached at all. Enabling them then makes the recorder and
// the audit report move.

#include "engine_host_support.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/harness_handler.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/script_runner.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace harness = tessera::harness;
namespace automation = tessera::automation;
namespace testing = tessera::harness::testing;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "diagnostics_cost: " << message << "\n";
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

class MemorySink final : public harness::ArtifactSink {
public:
    [[nodiscard]] std::expected<void, harness::ProtocolError>
    write(std::string_view /*path*/, std::string_view bytes) override {
        last_bytes = bytes;
        return {};
    }

    std::string last_bytes;
};

constexpr std::string_view kScript = R"({
    "scriptVersion": 1,
    "steps": [
        {"action": "snapshot"},
        {"action": "click", "uiId": "9003"},
        {"action": "type", "uiId": "9004", "text": "Ada"}
    ]
})";

std::string run_script(testing::HostFixture& scene, harness::ScriptReport& report) {
    auto parsed = harness::parse_script(kScript);
    if (!expect(parsed.has_value(), "the script parses")) {
        return {};
    }
    MemorySink sink;
    harness::HarnessCommandHandler handler(scene.automation, sink);
    ZeroClock clock;
    harness::ScriptRunner runner(std::move(*parsed), handler, sink, clock);
    while (runner.pump()) {
    }
    report = runner.report();
    auto json = harness::to_json(encode_script_report(report));
    return json.has_value() ? std::move(*json) : std::string{};
}

} // namespace

int main() {
    // 1. Seams attached, diagnostics disabled: no recorder or registry call, no artifact work.
    std::string disabled_report;
    {
        testing::HostFixture scene;
        tessera::diagnostics::TraceRecorder recorder(64);
        tessera::diagnostics::MetricsRegistry metrics;
        automation::AutomationDiagnostics hooks;
        hooks.trace = &recorder;
        hooks.metrics = &metrics;
        hooks.enabled = false;
        scene.automation.set_diagnostics(hooks);

        harness::ScriptReport report;
        disabled_report = run_script(scene, report);
        expect(report.passed, "the disabled-diagnostics run still passes");
        expect(report.steps_executed == 3, "all three steps ran");
        expect(recorder.size() == 0, "a disabled recorder received no event");
        expect(metrics.size() == 0, "a disabled registry received no metric");
        const auto& stats = scene.automation.stats();
        expect(stats.trace_events == 0, "the host recorded no trace event while disabled");
        expect(stats.audit_reports == 0, "the host built no audit report while disabled");

        // A trace request still answers, with the documented empty document and no recording.
        const auto trace = scene.automation.collect_trace();
        expect(trace.has_value() && trace->json == "[]" && trace->span_count == 0,
               "a disabled trace is the empty Chrome document");
        expect(recorder.size() == 0, "collecting a disabled trace recorded nothing");

        // An audit request reports the disabled state instead of fabricating a score.
        const auto audit = scene.automation.run_audit("desktop");
        expect(!audit.has_value() && audit.error().code == harness::ProtocolErrorCode::Internal,
               "a disabled audit is an internal diagnostic");
        expect(metrics.size() == 0 && scene.automation.stats().audit_reports == 0,
               "a disabled audit built no report and no metric");
    }

    // 2. No seams at all: identical report bytes, so "attached but disabled" does no work.
    std::string bare_report;
    {
        testing::HostFixture scene;
        harness::ScriptReport report;
        bare_report = run_script(scene, report);
        expect(report.passed, "the no-seams run passes");
        expect(scene.automation.stats().trace_events == 0, "a bare host records no trace event");
    }
    expect(!disabled_report.empty() && disabled_report == bare_report,
           "disabled diagnostics produce the same report bytes as no diagnostics");

    // 3. Enabled: the seams move and the audit report is produced from them.
    {
        testing::HostFixture scene;
        tessera::diagnostics::TraceRecorder recorder(64);
        tessera::diagnostics::MetricsRegistry metrics;
        const std::size_t slow_frames = metrics.add_counter("automation.test_frames");
        metrics.increment(slow_frames, 7);
        automation::AutomationDiagnostics hooks;
        hooks.trace = &recorder;
        hooks.metrics = &metrics;
        hooks.enabled = true;
        scene.automation.set_diagnostics(hooks);

        harness::ActionCommand click;
        click.kind = harness::ActionKind::Click;
        click.ui_id = automation::kFixtureButtonId;
        expect(scene.automation.dispatch_action(click).has_value(), "the click dispatches");
        expect(scene.automation.stats().trace_events == 1, "the host counted the span");

#if defined(TESSERA_ENABLE_TRACING)
        // Span contents only exist where the recorder is compiled in; with the option off the
        // recorder is a no-op and `collect_trace` answers the empty document instead
        // (tracing_strip_tests covers that flavour).
        expect(recorder.size() == 1, "an enabled action records exactly one span");
        const auto trace = scene.automation.collect_trace();
        expect(trace.has_value() && trace->span_count == 2, "the trace includes its own request");
        expect(trace.has_value() && trace->json.find("automation.click") != std::string::npos,
               "the trace carries the action span");
#else
        expect(recorder.size() == 0, "a tracing-off build records no span");
        const auto trace = scene.automation.collect_trace();
        expect(trace.has_value() && trace->json == "[]" && trace->span_count == 0,
               "a tracing-off build answers the empty document");
#endif

        const auto audit = scene.automation.run_audit("desktop");
        expect(audit.has_value() && audit->passed && audit->score == 100.0,
               "the enabled audit passes on a clean run");
        expect(audit.has_value() &&
                   audit->json.find("\"name\":\"automation.test_frames\"") != std::string::npos,
               "the audit embeds the metrics registry");
        expect(audit.has_value() && audit->json.find("\"value\":7") != std::string::npos,
               "the audit carries the registered value");
        expect(scene.automation.stats().audit_reports == 1, "the host built one audit report");
    }

    if (failures != 0) {
        std::cerr << "diagnostics_cost: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "diagnostics_cost: ok\n";
    return 0;
}
