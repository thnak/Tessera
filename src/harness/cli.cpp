// SPDX-License-Identifier: MIT

#include "tessera/harness/cli.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <string_view>

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

} // namespace

std::string_view cli_usage() noexcept {
    return "tessera - deterministic headless UI harness\n"
           "\n"
           "Usage:\n"
           "  tessera test --script <path> [--mode structural|golden|gpu-smoke|audit]\n"
           "               [--out <dir>] [--report <path>]\n"
           "  tessera headless --mode <mode> [--profile <name>] [--script <path>]\n"
           "                   [--out <dir>]\n"
           "  tessera --help | --version\n"
           "\n"
           "Exit codes: 0 pass, 1 assertion/audit failure, 2 invalid script/config.\n"
           "Status: the JSON-lines codec, stdio adapter, and session loop are implemented;\n"
           "the script runner behind --script is not yet available.\n";
}

int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
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

    // Values are validated even though the runner is not wired yet: a later slice must not inherit
    // a parser that accepted an empty directory or report path.
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
    // The parser and the loop exist, the runner that would execute a script does not. Reject
    // explicitly so an agent cannot mistake a no-op for a pass (section 3 exit code 2).
    err << "tessera " << command << ": --script is not implemented yet "
        << "(docs/specs/headless-automation-v1.md section 3)\n";
    return 2;
}

} // namespace tessera::harness
