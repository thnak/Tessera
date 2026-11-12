// SPDX-License-Identifier: MIT
//
// `tessera test --script` with `app.config`: the scene comes from the project's generated
// automation manifest, scripts address the manifest uiIds, and a bad config or manifest is exit 2.
// Without `app.config` the fixture scene (ids 9001-9010) is still the default.

#include "tessera/harness/cli.hpp"
#include "tessera/harness/in_process_transport.hpp"

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
        std::cerr << "cli_app_scene: " << message << "\n";
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
    std::vector<std::string_view> views(args.begin(), args.end());
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

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // The CLI only accepts working-directory-relative paths, so stage the fixture app locally.
    const fs::path source = fs::path(std::string(kFixtureDirectory));
    const fs::path project = "harness_app_fixture";
    std::error_code ignored;
    fs::remove_all(project, ignored);
    fs::remove_all("cli_app_out", ignored);
    fs::create_directories(project / "generated");
    fs::copy_file(source / "app/tessera.json", project / "tessera.json");
    fs::copy_file(source / "app/generated/automation.manifest.json",
                  project / "generated/automation.manifest.json");
    const std::string script = (source / "app-smoke.json").string();

    // Serving with --app builds the manifest scene; an immediately closed transport ends cleanly.
    {
        tessera::harness::InProcessPair pair = tessera::harness::make_in_process_pair();
        pair.second->close();
        std::vector<std::string_view> serve_args{"headless", "--mode", "structural", "--app",
                                                 "harness_app_fixture/tessera.json"};
        std::ostringstream out;
        std::ostringstream err;
        const int code = tessera::harness::run_cli(serve_args, out, err, pair.first.get());
        expect(code == 0, "serving the manifest scene ends cleanly on EOF");
        std::vector<std::string_view> bad_args{"headless", "--mode", "structural", "--app",
                                               "nope/tessera.json"};
        tessera::harness::InProcessPair other = tessera::harness::make_in_process_pair();
        other.second->close();
        std::ostringstream bad_err;
        expect(tessera::harness::run_cli(bad_args, out, bad_err, other.first.get()) == 2 &&
                   contains(bad_err.str(), "ConfigUnreadable"),
               "a missing --app config is exit 2");
    }
    expect(run({"test", "--script", script, "--app", "harness_app_fixture/tessera.json"}).code == 2,
           "--app is rejected together with --script");

    const Outcome first = run({"test", "--script", script, "--out", "cli_app_out"});
    expect(first.code == 0, "the app script passes against the manifest scene");
    const std::string report = read_file("cli_app_out/report.json");
    expect(contains(report, "7000000000000000003"), "the report names the manifest button id");
    expect(contains(report, "Ada"), "typing reached the manifest TextInput");
    expect(contains(report, "\"kind\":\"Button\"") || contains(report, "Button"),
           "the snapshot carries the manifest kind");

    const Outcome again = run({"test", "--script", script, "--out", "cli_app_out"});
    expect(again.code == 0 && read_file("cli_app_out/report.json") == report,
           "app runs are byte-identical");

    // A fixture id is not part of the app scene.
    const Outcome fixture_id =
        run({"test", "--script", std::string(kFixtureDirectory) + "/smoke.json", "--out",
             "cli_app_out"});
    expect(fixture_id.code == 0, "no app.config keeps the fixture default");

    fs::remove(project / "generated/automation.manifest.json");
    const Outcome missing = run({"test", "--script", script, "--out", "cli_app_out"});
    expect(missing.code == 2, "a missing manifest is exit 2");
    expect(contains(missing.err, "ManifestMissing"), "the error names ManifestMissing");

    {
        std::ofstream bad(project / "generated/automation.manifest.json");
        bad << "{\"manifestVersion\":1";
    }
    const Outcome malformed = run({"test", "--script", script, "--out", "cli_app_out"});
    expect(malformed.code == 2 && contains(malformed.err, "ManifestInvalid"),
           "a malformed manifest is exit 2");

    fs::remove(project / "tessera.json");
    const Outcome no_config = run({"test", "--script", script, "--out", "cli_app_out"});
    expect(no_config.code == 2 && contains(no_config.err, "ConfigUnreadable"),
           "a missing tessera.json is exit 2");

    fs::remove_all(project, ignored);
    fs::remove_all("cli_app_out", ignored);
    return failures == 0 ? 0 : 1;
}
