// SPDX-License-Identifier: MIT

#include "tessera/harness/cli.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

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
    outcome.code = tessera::harness::run_cli(args, out, err);
    outcome.out = out.str();
    outcome.err = err.str();
    return outcome;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main() {
    // --help and --version are the two commands that work today.
    {
        const Outcome help = run({"--help"});
        expect(help.code == 0, "top-level help exits 0");
        expect(contains(help.out, "Usage:") && contains(help.out, "tessera test"),
               "help describes both commands");
        expect(contains(help.out, "Exit codes"), "help documents the exit codes");
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

    // No subcommand is invalid input, not a silent success.
    {
        const Outcome none = run({});
        expect(none.code == 2, "missing subcommand exits 2");
        expect(contains(none.err, "Usage:"), "missing subcommand prints usage");
    }

    // --script parses but the runner is not implemented yet: reject loudly with exit code 2.
    {
        const Outcome test = run({"test", "--script", "tests/agent/login.json"});
        expect(test.code == 2, "--script exits 2");
        expect(contains(test.err, "not implemented"), "--script says it is not implemented");
        expect(contains(test.err, "headless-automation-v1"), "--script points at the spec");

        const Outcome full = run({"test", "--script", "in.json", "--mode", "golden", "--out", "out",
                                  "--report", "out/report.json"});
        expect(full.code == 2 && contains(full.err, "not implemented"),
               "fully specified test is still not implemented");

        const Outcome headless =
            run({"headless", "--mode", "audit", "--profile", "desktop", "--script", "audit.json"});
        expect(headless.code == 2 && contains(headless.err, "not implemented"),
               "headless --script rejected");

        const Outcome headless_no_script = run({"headless", "--mode", "structural"});
        expect(headless_no_script.code == 2 && contains(headless_no_script.err, "not implemented"),
               "headless without a script is not implemented either");
    }

    // Missing or malformed configuration is exit code 2 with a specific diagnostic.
    {
        expect(run({"test"}).code == 2 && contains(run({"test"}).err, "--script is required"),
               "test requires a script");
        expect(run({"headless"}).code == 2 && contains(run({"headless"}).err, "--mode is required"),
               "headless requires a mode");
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
        for (const char* mode : {"structural", "golden", "gpu-smoke", "audit"}) {
            const Outcome outcome = run({"headless", "--mode", mode});
            expect(outcome.code == 2 && contains(outcome.err, "not implemented"),
                   "documented mode parses");
        }
    }

    // Repeating an option keeps the last value; the parser does not silently ignore arguments.
    {
        const Outcome repeated = run({"test", "--script", "a.json", "--script", "b.json"});
        expect(repeated.code == 2 && contains(repeated.err, "not implemented"),
               "last --script value wins");
        expect(run({"test", "--script", "x", "--report", ""}).code == 2, "empty report rejected");
    }

    if (failures != 0) {
        std::cerr << "harness_cli: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
