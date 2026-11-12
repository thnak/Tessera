// SPDX-License-Identifier: MIT
//
// The promoted CLI end to end: `tessera test --script` and `tessera headless --script` run a real
// script against the windowless fixture, write the section 5 artifacts, and return the documented
// exit codes (0 pass, 1 run failure, 2 invalid script/config). Two identical runs produce
// byte-identical reports.

#include "tessera/harness/cli.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kFixtureDirectory = TESSERA_HARNESS_FIXTURE_SCRIPT_DIR;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cli_integration: " << message << "\n";
        ++failures;
    }
    return condition;
}

struct Outcome {
    int code = 0;
    std::string out;
    std::string err;
};

Outcome run(const std::vector<std::string>& args) {
    std::vector<std::string_view> views;
    views.reserve(args.size());
    for (const std::string& argument : args) {
        views.emplace_back(argument);
    }
    std::ostringstream out;
    std::ostringstream err;
    Outcome outcome;
    outcome.code = tessera::harness::run_cli(views, out, err);
    outcome.out = out.str();
    outcome.err = err.str();
    return outcome;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::string fixture(std::string_view name) {
    return std::string(kFixtureDirectory) + "/" + std::string(name);
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    const std::string smoke = fixture("smoke.json");
    for (const char* directory : {"cli_it", "cli_it_fail", "cli_it_headless", "cli_it_custom"}) {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }

    // Happy path: eight steps, the documented artifacts, exit 0.
    const Outcome first = run({"test", "--script", smoke, "--out", "cli_it"});
    expect(first.code == 0, "the smoke script passes");
    expect(contains(first.out, "pass"), "stdout reports the pass");
    expect(contains(first.out, "8/8 steps"), "stdout reports the step count");
    expect(contains(first.out, "cli_it/report.json"), "stdout names the report");
    expect(fs::exists("cli_it/report.json"), "the report was written");
    const std::string report = read_file("cli_it/report.json");
    expect(contains(report, "\"passed\":true"), "the report says passed");
    expect(contains(report, "\"stepsRun\":8"), "the report counts eight steps");
    expect(contains(report, "\"action\":\"snapshot\""), "the report lists the snapshot step");
    expect(contains(report, "Count: 1"), "the report embeds the post-click snapshot");
    expect(fs::exists("cli_it/trace.json"), "the trace artifact was written");
    expect(fs::exists("cli_it/audit.json"), "the audit artifact was written");
    expect(contains(read_file("cli_it/audit.json"), "\"profile\":\"desktop\""),
           "the audit artifact names the profile");

    // Determinism: the same invocation produces the same report bytes.
    const Outcome again = run({"test", "--script", smoke, "--out", "cli_it"});
    expect(again.code == 0, "the second run passes");
    expect(read_file("cli_it/report.json") == report, "reports are byte-identical");

    // A runtime failure is exit 1 and keeps the protocol code visible.
    const Outcome target =
        run({"test", "--script", fixture("missing-target.json"), "--out", "cli_it_fail"});
    expect(target.code == 1, "an unknown target is a run failure");
    expect(contains(target.err, "uiIdNotFound"), "the failure names the protocol code");
    expect(fs::exists("cli_it_fail/report.json"), "a failing run still writes its report");

    // A script the lane cannot execute is exit 2 and never starts the engine.
    const Outcome unsupported =
        run({"test", "--script", fixture("unsupported-action.json"), "--out", "cli_it_custom"});
    expect(unsupported.code == 2, "an unsupported action is invalid input");
    expect(contains(unsupported.err, "unsupportedAction"), "the code is unsupportedAction");
    expect(!fs::exists("cli_it_custom/report.json"), "no report for a rejected script");

    // --report overrides the default report location.
    const Outcome custom = run(
        {"test", "--script", smoke, "--out", "cli_it", "--report", "cli_it_custom/report.json"});
    expect(custom.code == 0, "the custom report run passes");
    expect(fs::exists("cli_it_custom/report.json"), "the custom report path is honoured");

    // `tessera headless --mode audit` runs the same script and enables the diagnostics seams.
    const Outcome headless = run({"headless", "--mode", "audit", "--profile", "ci", "--script",
                                  smoke, "--out", "cli_it_headless"});
    expect(headless.code == 0, "headless audit mode passes");
    expect(fs::exists("cli_it_headless/trace.json") && fs::exists("cli_it_headless/audit.json"),
           "headless wrote the diagnostics artifacts");

    for (const char* directory : {"cli_it", "cli_it_fail", "cli_it_headless", "cli_it_custom"}) {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }

    if (failures != 0) {
        std::cerr << "cli_integration: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "cli_integration: ok\n";
    return 0;
}
