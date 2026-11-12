// SPDX-License-Identifier: MIT

#include "cli_internal.hpp"
#include "tessera/app/host.hpp"
#include "tessera/automation/app_config.hpp"
#include "tessera/automation/engine_host.hpp"
#include "tessera/automation/fixture_scene.hpp"
#include "tessera/automation/manifest_scene.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/cli_frame.hpp"
#include "tessera/harness/harness_handler.hpp"
#include "tessera/harness/headless_serve.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/latency_audit.hpp"
#include "tessera/harness/protocol.hpp"
#include "tessera/harness/script.hpp"
#include "tessera/harness/script_runner.hpp"
#include "tessera/harness/session.hpp"
#include "tessera/harness/stdio_transport.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::harness::detail {

void print_script_error(std::ostream& err, std::string_view command, const ScriptError& error) {
    err << "tessera " << command << ": " << to_string(error.code) << ": " << error.message;
    if (!error.detail.empty()) {
        err << " (" << error.detail << ")";
    }
    if (error.protocol_code.has_value()) {
        err << " [" << to_string(*error.protocol_code) << "]";
    }
    if (error.step != kNoScriptStep) {
        err << " at step " << error.step;
    }
    err << "\n";
}

// Simulated session clock for the script lane: it advances by one fixed tick per pump, so step
// durations are deterministic and never read the wall clock (spec section 7).
class TickClock final : public SessionClock {
public:
    [[nodiscard]] std::uint64_t now_us() const override {
        return now_us_;
    }
    void advance(std::uint64_t micros) {
        now_us_ += micros;
    }

private:
    std::uint64_t now_us_ = 0;
};

constexpr std::uint64_t kTickMicros = 16'667; // 1/60 s, rounded down
constexpr double kTickSeconds = 1.0 / 60.0;
constexpr std::size_t kMaxScriptTicks = kMaxScriptSteps + 1;

[[nodiscard]] bool wants_diagnostics(const Script& script) {
    for (const ScriptStep& step : script.steps) {
        if (step.action == ScriptAction::Trace || step.action == ScriptAction::Audit) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<std::string> read_script_file(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.eof() && file.fail()) {
        return std::nullopt;
    }
    return text;
}

// Runs one parsed script against the built-in windowless fixture. Returns the documented section 3
// exit code: 0 pass, 1 run failure, 2 configuration/artifact failure.
[[nodiscard]] int run_script(const Script& script, std::string_view command,
                             std::string_view output_directory, const std::string& report_path,
                             bool diagnostics_enabled, const std::optional<LatencyProfile>& latency,
                             SessionClock& audit_clock, const automation::AppManifest* app,
                             const std::optional<CliFrameRequest>& frame_request, std::ostream& out,
                             std::ostream& err) {
    ui::Node root(1);
    // The fixture is the default scene; an app manifest (script `app.config`) replaces it.
    std::optional<automation::FixtureScene> fixture;
    std::optional<automation::ManifestScene> manifest_scene;
    if (app != nullptr) {
        manifest_scene.emplace(root, *app);
    } else {
        fixture.emplace(root);
    }
    const std::string stylesheet = app != nullptr
                                       ? automation::ManifestScene::stylesheet(*app)
                                       : std::string(automation::FixtureScene::stylesheet());

    runtime::ManualClock clock;
    runtime::RuntimeLoop loop(clock);
    app::HostConfig config;
    config.width = static_cast<float>(script.profile.width);
    config.height = static_cast<float>(script.profile.height);
    config.dpi_scale = static_cast<float>(script.profile.scale);
    app::AppHost host(root, ui::style::compile_stylesheet(stylesheet), loop, config);
    // One frame before the first step so layout bounds (and therefore hit testing) are valid.
    (void)host.begin_frame(0.0);
    host.end_frame();

    diagnostics::TraceRecorder trace;
    diagnostics::MetricsRegistry metrics;
    automation::EngineAutomationHost::Config host_config;
    if (app != nullptr) {
        host_config.annotations = automation::annotations_from_manifest(*app);
    }
    if (manifest_scene.has_value()) {
        host_config.virtual_collections = &manifest_scene->virtual_collections();
    }
    automation::EngineAutomationHost automation_host(root, host, loop, std::move(host_config));
    automation::AutomationDiagnostics hooks;
    hooks.trace = &trace;
    hooks.metrics = &metrics;
    hooks.enabled = diagnostics_enabled;
    automation_host.set_diagnostics(hooks);

    FileArtifactSink sink;
    HarnessHandlerConfig handler_config;
    handler_config.output_directory = std::string(output_directory);
    HarnessCommandHandler handler(automation_host, sink, handler_config);

    TickClock script_clock;
    // Audit mode alone measures real elapsed time (spec section 7); every other mode keeps the
    // simulated tick clock so its reports stay byte-identical across runs.
    SessionClock& runner_clock = latency.has_value() ? audit_clock : script_clock;
    ScriptRunner runner(script, handler, sink, runner_clock);

    // One RuntimeLoop tick per pump: one pump is one bounded step (spec sections 6.3 and 6.6).
    std::size_t ticks = 0;
    while (!runner.finished() && ticks < kMaxScriptTicks) {
        clock.advance(kTickSeconds);
        const runtime::RuntimeTick tick = loop.tick();
        (void)tick;
        const bool more = runner.pump();
        script_clock.advance(kTickMicros);
        ++ticks;
        if (!more) {
            break;
        }
    }
    if (!runner.finished()) {
        err << "tessera " << command << ": script did not settle within " << kMaxScriptTicks
            << " ticks\n";
        return 2;
    }

    const ScriptReport& report = runner.report();
    JsonValue report_json = encode_script_report(report);
    std::optional<LatencyAudit> latency_audit;
    if (latency.has_value()) {
        latency_audit = evaluate_latency(report, *latency);
        const JsonValue encoded = encode_latency_audit(*latency_audit);
        report_json.set("latencyAudit", encoded);
        auto audit_document = to_json(encoded);
        const std::string audit_path =
            std::string(output_directory) + "/" + std::string(kLatencyAuditFile);
        if (!audit_document.has_value() || !sink.write(audit_path, *audit_document).has_value()) {
            err << "tessera " << command << ": cannot write '" << audit_path << "'\n";
            return 2;
        }
    }
    // Golden / gpu-smoke: the script ran structurally above; now render its final scene state and
    // compare it. A failing script keeps exit 1 and skips the render (the frame would be moot).
    int frame_exit = 0;
    if (frame_request.has_value() && report.passed) {
        frame_exit = run_cli_frame_check(*frame_request, root, stylesheet, output_directory, sink,
                                         report_json, command, err);
    }
    auto document = to_json(report_json);
    if (!document.has_value()) {
        err << "tessera " << command << ": report could not be encoded\n";
        return 2;
    }
    if (!is_safe_artifact_path(report_path)) {
        err << "tessera " << command << ": --report must be a safe relative path\n";
        return 2;
    }
    if (auto written = sink.write(report_path, *document); !written.has_value()) {
        err << "tessera " << command << ": cannot write '" << report_path
            << "': " << written.error().message << "\n";
        return 2;
    }

    out << "tessera " << command << ": " << (report.passed ? "pass" : "fail") << ", "
        << report.steps_executed << "/" << report.steps_total << " steps, " << report.elapsed_us
        << " us, report " << report_path << "\n";
    if (latency_audit.has_value()) {
        out << "tessera " << command << ": latency audit '" << latency_audit->profile.name << "' "
            << (latency_audit->passed ? "pass" : "fail") << ", p95 " << latency_audit->stats.p95_us
            << " us, max " << latency_audit->stats.max_us << " us\n";
        for (const LatencyViolation& violation : latency_audit->violations) {
            err << "tessera " << command << ": latency violation " << violation.id << ": "
                << violation.actual_us << " us > " << violation.limit_us << " us\n";
        }
    }
    if (report.passed) {
        if (frame_exit == 2) {
            return 2;
        }
        return (latency_audit.has_value() && !latency_audit->passed) || frame_exit != 0 ? 1 : 0;
    }
    if (report.error.has_value()) {
        print_script_error(err, command, *report.error);
    }
    for (const ScriptStepReport& step : report.steps) {
        if (!step.ok) {
            print_script_error(err, command, step.error);
            break;
        }
    }
    return 1;
}

} // namespace tessera::harness::detail
