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
#include "tessera/harness/cli.hpp"
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

namespace tessera::harness {
using detail::print_script_error;
using detail::read_script_file;
using detail::run_script;
using detail::wants_diagnostics;

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

} // namespace

std::string_view cli_usage() noexcept {
    return "tessera - deterministic headless UI harness\n"
           "\n"
           "Usage:\n"
           "  tessera test --script <path> [--mode structural|audit] [--out <dir>] [--report "
           "<path>]\n"
           "  tessera headless --mode <mode> --script <path> [--out <dir>] [--report <path>]\n"
           "  tessera headless --mode <mode> [--app <tessera.json>]   (serve the stdio protocol)\n"
           "  tessera test --script <path> --mode golden|gpu-smoke --baseline <png> "
           "[--update-goldens] [--font <ttf>]\n"
           "  tessera --help | --version\n"
           "\n"
           "Exit codes: 0 pass, 1 run/audit failure, 2 invalid script/config.\n"
           "The script lane runs against the built-in windowless fixture scene and writes\n"
           "out/report.json (or --report) plus any snapshot/trace/audit artifacts the script asks\n"
           "for. --mode golden and gpu-smoke then render the final scene and compare it with "
           "--baseline;\n"
           "golden needs the software (lavapipe) Vulkan path, and only golden honours\n"
           "--update-goldens.\n";
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
    std::optional<std::string_view> baseline;
    std::optional<std::string_view> font;
    std::optional<std::string_view> app_path;
    bool update_goldens = false;

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
        } else if (argument == "--baseline") {
            const auto value = take_value(argument);
            if (!value.has_value()) {
                return 2;
            }
            baseline = *value;
        } else if (argument == "--app") {
            const auto value = take_value(argument);
            if (!value.has_value()) {
                return 2;
            }
            app_path = *value;
        } else if (argument == "--font") {
            const auto value = take_value(argument);
            if (!value.has_value()) {
                return 2;
            }
            font = *value;
        } else if (argument == "--update-goldens") {
            update_goldens = true;
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

    // Structural and audit modes drive the fixture scene without a GPU. Golden and gpu-smoke run
    // the same script, then render the final scene and compare it with `--baseline` (ADR-009):
    // `--update-goldens` rewrites the baseline from the software path only, never from gpu-smoke.
    // An audit-mode run enables the diagnostics seams for the script's audit steps.
    const CliMode mode = mode_text.has_value() ? *parse_mode(*mode_text) : CliMode::Structural;
    const bool frame_mode = mode == CliMode::Golden || mode == CliMode::GpuSmoke;
    if (!frame_mode && (baseline.has_value() || update_goldens)) {
        err << "tessera " << command
            << ": --baseline and --update-goldens apply to --mode golden or gpu-smoke only\n";
        return 2;
    }
    if (frame_mode && !script.has_value()) {
        err << "tessera " << command << ": --mode " << *mode_text << " needs --script\n";
        return 2;
    }
    if (frame_mode && !baseline.has_value()) {
        err << "tessera " << command << ": --mode " << *mode_text << " needs --baseline <png>\n";
        return 2;
    }
    if (baseline.has_value() && baseline->empty()) {
        err << "tessera: --baseline must not be empty\n";
        return 2;
    }
    if (update_goldens && mode != CliMode::Golden) {
        err << "tessera " << command
            << ": --update-goldens is only allowed with --mode golden (ADR-009)\n";
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
        std::optional<automation::AppManifest> serve_app;
        if (app_path.has_value()) {
            auto loaded = automation::load_app_manifest(artifact_path(*app_path));
            if (!loaded.has_value()) {
                err << "tessera " << command << ": " << automation::to_string(loaded.error().code)
                    << ": " << loaded.error().message << "\n";
                return 2;
            }
            serve_app = std::move(*loaded);
        }
        return run_headless_serve(*serve_transport, directory, mode == CliMode::Audit, nullptr, err,
                                  serve_app.has_value() ? &*serve_app : nullptr);
    }

    if (app_path.has_value()) {
        err << "tessera " << command << ": --app applies to serving without --script; scripts name "
            << "app.config themselves\n";
        return 2;
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
    std::optional<CliFrameRequest> frame_request;
    if (frame_mode) {
        CliFrameRequest request;
        request.mode = mode == CliMode::Golden ? CompareMode::Golden : CompareMode::GpuSmoke;
        request.baseline_path = std::string(*baseline);
        request.update_baseline = update_goldens;
        request.font_path = std::string(font.value_or(""));
        request.width = parsed->profile.width;
        request.height = parsed->profile.height;
        request.dpi_scale = static_cast<float>(parsed->profile.scale);
        frame_request = request;
    }
    SteadySessionClock steady_clock;
    return run_script(*parsed, command, directory, report_path,
                      mode == CliMode::Audit || wants_diagnostics(*parsed), latency,
                      audit_clock != nullptr ? *audit_clock : steady_clock,
                      app.has_value() ? &*app : nullptr, frame_request, out, err);
}

} // namespace tessera::harness
