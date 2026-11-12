// SPDX-License-Identifier: MIT

#include "tessera/app/host.hpp"
#include "tessera/automation/app_config.hpp"
#include "tessera/automation/engine_host.hpp"
#include "tessera/automation/fixture_scene.hpp"
#include "tessera/automation/manifest_scene.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/cli.hpp"
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

namespace tessera::harness {
namespace {

enum class CliMode {
    Structural,
    Golden,
    GpuSmoke,
    Audit,
};

[[nodiscard]] std::optional<CliMode> parse_mode(std::string_view text) noexcept {
    if (text == "structural") {
        return CliMode::Structural;
    }
    if (text == "golden") {
        return CliMode::Golden;
    }
    if (text == "gpu-smoke") {
        return CliMode::GpuSmoke;
    }
    if (text == "audit") {
        return CliMode::Audit;
    }
    return std::nullopt;
}

void print_version(std::ostream& out) {
    out << "tessera " << kHarnessCliVersion << "\nheadless automation protocol " << kProtocolVersion
        << "\n";
}

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
                             std::ostream& out, std::ostream& err) {
    ui::Node root(1);
    // The fixture is the default scene; an app manifest (script `app.config`) replaces it.
    std::optional<automation::FixtureScene> fixture;
    std::optional<automation::ManifestScene> manifest_scene;
    if (app != nullptr) {
        manifest_scene.emplace(root, *app);
    } else {
        fixture.emplace(root);
    }
    const std::string_view stylesheet = app != nullptr ? automation::ManifestScene::stylesheet()
                                                       : automation::FixtureScene::stylesheet();

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
    automation::EngineAutomationHost automation_host(root, host, loop);
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
        return latency_audit.has_value() && !latency_audit->passed ? 1 : 0;
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

} // namespace

std::string_view cli_usage() noexcept {
    return "tessera - deterministic headless UI harness\n"
           "\n"
           "Usage:\n"
           "  tessera test --script <path> [--mode structural|audit] [--out <dir>] [--report "
           "<path>]\n"
           "  tessera headless --mode <mode> --script <path> [--out <dir>] [--report <path>]\n"
           "  tessera --help | --version\n"
           "\n"
           "Exit codes: 0 pass, 1 run/audit failure, 2 invalid script/config.\n"
           "The script lane runs against the built-in windowless fixture scene and writes\n"
           "out/report.json (or --report) plus any snapshot/trace/audit artifacts the script asks\n"
           "for. --mode golden and --mode gpu-smoke need the rendering lanes and are rejected.\n";
}

static int run_cli_impl(std::span<const std::string_view> args, std::ostream& out,
                        std::ostream& err, LineTransport* serve_transport,
                        SessionClock* audit_clock);

int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    return run_cli_impl(args, out, err, nullptr, nullptr);
}

int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
            LineTransport* serve_transport) {
    return run_cli_impl(args, out, err, serve_transport, nullptr);
}

int run_cli_with_audit_clock(std::span<const std::string_view> args, std::ostream& out,
                             std::ostream& err, SessionClock* audit_clock) {
    return run_cli_impl(args, out, err, nullptr, audit_clock);
}

static int run_cli_impl(std::span<const std::string_view> args, std::ostream& out,
                        std::ostream& err, LineTransport* serve_transport,
                        SessionClock* audit_clock) {
    if (args.empty()) {
        err << cli_usage();
        return 2;
    }
    if (args.front() == "--help" || args.front() == "-h") {
        out << cli_usage();
        return 0;
    }
    if (args.front() == "--version" || args.front() == "-V") {
        print_version(out);
        return 0;
    }

    const std::string_view command = args.front();
    if (command != "test" && command != "headless") {
        err << "tessera: unknown command '" << command << "'\n\n" << cli_usage();
        return 2;
    }

    std::optional<std::string_view> script;
    std::optional<std::string_view> mode_text;
    std::optional<std::string_view> out_directory;
    std::optional<std::string_view> report;
    std::optional<std::string_view> profile;

    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string_view argument = args[index];
        if (argument == "--help" || argument == "-h") {
            out << cli_usage();
            return 0;
        }
        if (argument == "--version" || argument == "-V") {
            print_version(out);
            return 0;
        }
        const auto take_value = [&](std::string_view name) -> std::optional<std::string_view> {
            if (index + 1 >= args.size()) {
                err << "tessera: " << name << " requires a value\n";
                return std::nullopt;
            }
            ++index;
            return args[index];
        };
        if (argument == "--script") {
            const auto value = take_value(argument);
            if (!value.has_value()) {
                return 2;
            }
            script = *value;
        } else if (argument == "--mode") {
            const auto value = take_value(argument);
            if (!value.has_value()) {
                return 2;
            }
            mode_text = *value;
        } else if (argument == "--out") {
            const auto value = take_value(argument);
            if (!value.has_value()) {
                return 2;
            }
            out_directory = *value;
        } else if (argument == "--report") {
            const auto value = take_value(argument);
            if (!value.has_value()) {
                return 2;
            }
            report = *value;
        } else if (argument == "--profile") {
            const auto value = take_value(argument);
            if (!value.has_value()) {
                return 2;
            }
            profile = *value;
        } else {
            err << "tessera: unknown option '" << argument << "'\n";
            return 2;
        }
    }

    if (out_directory.has_value() && out_directory->empty()) {
        err << "tessera: --out must not be empty\n";
        return 2;
    }
    if (report.has_value() && report->empty()) {
        err << "tessera: --report must not be empty\n";
        return 2;
    }
    if (profile.has_value() && profile->empty()) {
        err << "tessera: --profile must not be empty\n";
        return 2;
    }
    if (script.has_value() && script->empty()) {
        err << "tessera: --script must not be empty\n";
        return 2;
    }
    if (mode_text.has_value() && !parse_mode(*mode_text).has_value()) {
        err << "tessera: unknown mode '" << *mode_text << "'\n";
        return 2;
    }
    if (command == "headless" && !mode_text.has_value()) {
        err << "tessera headless: --mode is required\n";
        return 2;
    }
    if (command == "test" && !script.has_value()) {
        err << "tessera test: --script is required\n";
        return 2;
    }

    // Only the structural lane exists: scripts drive the fixture scene without a GPU. Golden and
    // gpu-smoke runs need the offscreen renderer, so they are rejected instead of pretending to
    // pass. An audit-mode run enables the diagnostics seams for the script's audit steps.
    const CliMode mode = mode_text.has_value() ? *parse_mode(*mode_text) : CliMode::Structural;
    if (mode == CliMode::Golden || mode == CliMode::GpuSmoke) {
        err << "tessera " << command << ": --mode " << *mode_text
            << " is not implemented yet (structural and audit run a script)\n";
        return 2;
    }

    // Windows users type `--out build\reports`; the artifact rules are written for POSIX
    // separators, so normalize before validating and keep the normalized form for the paths we
    // create.
    const auto artifact_path = [](std::string_view path) {
        std::string normalized(path);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        return normalized;
    };
    const std::string directory = artifact_path(out_directory.value_or("out"));
    if (!is_safe_artifact_path(directory)) {
        err << "tessera: --out must be a safe relative path below the working directory\n";
        return 2;
    }
    const std::string report_path =
        report.has_value() ? artifact_path(*report) : directory + "/report.json";
    if (!is_safe_artifact_path(report_path)) {
        err << "tessera: --report must be a safe relative path below the working directory\n";
        return 2;
    }

    if (!script.has_value()) {
        // `headless` without a script serves the stdio protocol until EOF. The fixture host stays
        // the scene until the app-config loader exists.
        std::unique_ptr<StdioTransport> owned;
        if (serve_transport == nullptr) {
            owned = std::make_unique<StdioTransport>(make_process_stdio_stream());
            serve_transport = owned.get();
        }
        return run_headless_serve(*serve_transport, directory, mode == CliMode::Audit, nullptr,
                                  err);
    }

    const auto text = read_script_file(*script);
    if (!text.has_value()) {
        err << "tessera " << command << ": cannot read script '" << *script << "'\n";
        return 2;
    }
    if (text->size() > kMaxScriptBytes) {
        err << "tessera " << command << ": script exceeds the " << kMaxScriptBytes << "-byte cap\n";
        return 2;
    }
    auto parsed = parse_script(*text);
    if (!parsed.has_value()) {
        print_script_error(err, command, parsed.error());
        return 2;
    }
    std::optional<LatencyProfile> latency;
    if (mode == CliMode::Audit) {
        auto resolved = resolve_latency_profile(profile.value_or("desktop"));
        if (!resolved.has_value()) {
            err << "tessera " << command << ": " << resolved.error() << "\n";
            return 2;
        }
        latency = std::move(*resolved);
    }
    // `app.config` names the project's tessera.json; its generated manifest supplies the scene.
    // Without it the built-in fixture runs. A bad config or manifest is exit 2 (spec section 3).
    std::optional<automation::AppManifest> app;
    if (!parsed->app_config.empty()) {
        auto loaded = automation::load_app_manifest(artifact_path(parsed->app_config));
        if (!loaded.has_value()) {
            err << "tessera " << command << ": " << automation::to_string(loaded.error().code)
                << ": " << loaded.error().message << "\n";
            return 2;
        }
        app = std::move(*loaded);
    }
    SteadySessionClock steady_clock;
    return run_script(*parsed, command, directory, report_path,
                      mode == CliMode::Audit || wants_diagnostics(*parsed), latency,
                      audit_clock != nullptr ? *audit_clock : steady_clock,
                      app.has_value() ? &*app : nullptr, out, err);
}

} // namespace tessera::harness
