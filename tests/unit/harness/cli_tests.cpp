// SPDX-License-Identifier: MIT
//
// Argument handling of the promoted `tessera` CLI: documented flags, exit codes, and the exit-2
// diagnostics for an unreadable or invalid script. Execution behaviour lives in
// cli_integration_tests.

#include "tessera/harness/cli.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::harness::run_cli;

constexpr std::string_view kFixtureDirectory = TESSERA_HARNESS_FIXTURE_SCRIPT_DIR;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_cli: " << message << "\n";
        ++failures;
    }
    return condition;
}

struct Outcome {
    int code = 0;
    std::string out;
    std::string err;
};

Outcome run(const std::vector<std::string_view>& args) {
    std::ostringstream out;
    std::ostringstream err;
    Outcome outcome;
    outcome.code = run_cli(args, out, err);
    outcome.out = out.str();
    outcome.err = err.str();
    return outcome;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// Idles for `idle_waits` readiness waits, then delivers its lines and EOF.
class ScriptedTransport final : public tessera::harness::LineTransport {
public:
    using Error = tessera::harness::TransportError;
    using Code = tessera::harness::TransportErrorCode;

    [[nodiscard]] std::expected<std::string, Error> read_line() override {
        if (waits >= idle_waits) {
            if (next < inbound.size()) {
                return inbound[next++];
            }
            return std::unexpected(Error{Code::Closed, "eof"});
        }
        return std::unexpected(Error{Code::WouldBlock, "idle"});
    }
    [[nodiscard]] std::expected<void, Error> write_line(std::string_view line) override {
        outbound.emplace_back(line);
        return {};
    }
    [[nodiscard]] bool wait_readable(std::chrono::milliseconds) override {
        ++waits;
        return waits >= idle_waits;
    }
    [[nodiscard]] std::size_t max_line_bytes() const noexcept override {
        return 1U << 20U;
    }
    [[nodiscard]] bool closed() const noexcept override {
        return closed_state;
    }
    void close() noexcept override {
        closed_state = true;
    }

    std::vector<std::string> inbound;
    std::vector<std::string> outbound;
    std::size_t next = 0;
    std::size_t idle_waits = 0;
    std::size_t waits = 0;
    bool closed_state = false;
};

std::string fixture(std::string_view name) {
    return std::string(kFixtureDirectory) + "/" + std::string(name);
}

} // namespace

int main() {
    // --help and --version.
    {
        const Outcome help = run({"--help"});
        expect(help.code == 0, "top-level help exits 0");
        expect(contains(help.out, "Usage:") && contains(help.out, "tessera test"),
               "help describes both commands");
        expect(contains(help.out, "Exit codes"), "help documents the exit codes");
        expect(contains(help.out, "--script"), "help documents --script");
        expect(help.err.empty(), "help writes to stdout");

        const Outcome sub_help = run({"test", "--help"});
        expect(sub_help.code == 0 && contains(sub_help.out, "--script"), "subcommand help exits 0");

        const Outcome version = run({"--version"});
        expect(version.code == 0, "version exits 0");
        expect(contains(version.out, "0.1.0"), "version reports the harness version");
        expect(contains(version.out, "protocol 1"), "version reports the protocol version");
        expect(contains(run({"test", "--version"}).out, "0.1.0"),
               "version is accepted after a subcommand");
    }

    // `headless` without --script serves the protocol on the injected transport until EOF, with
    // one bounded wait per idle iteration and exit code 0.
    {
        ScriptedTransport transport;
        transport.inbound = {R"({"protocolVersion":1,"id":1,"cmd":"snapshot"})"};
        transport.idle_waits = 2;
        std::ostringstream out;
        std::ostringstream err;
        const std::vector<std::string_view> args{"headless", "--mode", "structural"};
        const int code = tessera::harness::run_cli(args, out, err, &transport);
        expect(code == 0, "headless serve exits 0 on EOF");
        expect(transport.outbound.size() == 1, "one request, one response");
        expect(!transport.outbound.empty() &&
                   contains(transport.outbound.front(), "\"id\":\"1\"") &&
                   contains(transport.outbound.front(), "\"ok\":true"),
               "the snapshot request was answered against the fixture host");
        expect(transport.waits == 2, "idle iterations waited instead of spinning");
    }

    // No subcommand is invalid input, not a silent success.
    {
        const Outcome none = run({});
        expect(none.code == 2, "missing subcommand exits 2");
        expect(contains(none.err, "Usage:"), "missing subcommand prints usage");
    }

    // Missing or malformed configuration is exit code 2 with a specific diagnostic.
    {
        expect(run({"test"}).code == 2 && contains(run({"test"}).err, "--script is required"),
               "test requires a script");
        const Outcome headless = run({"headless"});
        expect(headless.code == 2 && contains(headless.err, "--mode is required"),
               "headless requires a mode");
        expect(run({"headless", "--mode", "structural", "--out", "../escape"}).code == 2,
               "serve mode still validates --out");
        expect(run({"test", "--mode", "bogus", "--script", "x"}).code == 2,
               "unknown mode rejected");
        expect(run({"test", "--bogus"}).code == 2, "unknown option rejected");
        expect(run({"frobnicate"}).code == 2, "unknown command rejected");
        expect(run({"test", "--script"}).code == 2, "missing option value rejected");
        expect(run({"test", "--script", "x", "--out", ""}).code == 2,
               "empty output directory rejected");
        expect(run({"test", "--script", ""}).code == 2, "empty script rejected");
        expect(run({"headless", "--mode", "audit", "--profile", ""}).code == 2,
               "empty profile rejected");
        expect(run({"test", "--script", fixture("smoke.json"), "--report", ""}).code == 2,
               "empty report rejected");
    }

    // Only the structural lane exists; the rendering modes are rejected instead of pretending.
    for (const char* mode : {"golden", "gpu-smoke"}) {
        const Outcome outcome = run({"test", "--script", fixture("smoke.json"), "--mode", mode});
        expect(outcome.code == 2 && contains(outcome.err, "not implemented"),
               "documented rendering mode parses but is not implemented");
    }
    for (const char* mode : {"structural", "audit"}) {
        const Outcome outcome = run({"headless", "--mode", mode, "--script", "missing.json"});
        expect(outcome.code == 2 && contains(outcome.err, "cannot read script"),
               "documented script mode parses and reports a missing file");
    }

    // Script and artifact paths are validated before the engine starts.
    {
        const Outcome missing = run({"test", "--script", "does/not/exist.json"});
        expect(missing.code == 2 && contains(missing.err, "cannot read script"),
               "an unreadable script is exit 2");

        const Outcome absolute =
            run({"test", "--script", fixture("smoke.json"), "--out", "C:/tmp/harness"});
        expect(absolute.code == 2 && contains(absolute.err, "safe relative path"),
               "an absolute output directory is rejected");

        const Outcome version = run({"test", "--script", fixture("bad-version.json")});
        expect(version.code == 2 && contains(version.err, "unsupportedVersion"),
               "an unsupported script version is reported");

        const Outcome unsupported = run({"test", "--script", fixture("unsupported-action.json")});
        expect(unsupported.code == 2 && contains(unsupported.err, "unsupportedAction"),
               "a spec-only action is reported at parse time");
    }

    // Repeating an option keeps the last value; the parser does not silently ignore arguments.
    {
        const std::string smoke = fixture("smoke.json");
        const Outcome repeated = run({"test", "--script", smoke, "--script", "missing.json"});
        expect(repeated.code == 2 && contains(repeated.err, "cannot read script"),
               "the last --script value wins");
    }

    if (failures != 0) {
        std::cerr << "harness_cli: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "harness_cli: ok\n";
    return 0;
}
