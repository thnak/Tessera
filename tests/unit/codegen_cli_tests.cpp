// SPDX-License-Identifier: MIT

// Codegen CLI and incremental regeneration (Big Step C): run the real executable twice over the
// committed sample screen, assert the second run is a byte/mtime no-op, developer regions are
// preserved, --check reports drift, outside-region edits are overwritten, and exit codes are the
// documented 0/1/2.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32) && !defined(TESSERA_CLI_TESTS_NO_SUBPROCESS)
#include <sys/wait.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// iOS and some sandboxed Apple targets do not expose `system()`; the desktop CLIs are the only
// place the real executable is run, so those targets skip with a printed reason instead of failing
// to build. CMake sets TESSERA_NO_SUBPROCESS for iOS/tvOS/watchOS; the TargetConditionals check is
// the fallback for direct Xcode builds.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(TESSERA_NO_SUBPROCESS) ||                                                              \
    (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define TESSERA_CLI_TESTS_NO_SUBPROCESS 1
#endif

#if !defined(TESSERA_CLI_TESTS_NO_SUBPROCESS)
namespace {

namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

int run_cli(const fs::path& cli, const std::string& arguments, const fs::path& log_dir) {
    const fs::path log = log_dir / "cli.log";
#ifdef _WIN32
    // cmd strips a leading quote from the whole /c command, so invoke through `call`.
    const std::string prefix = "call ";
#else
    const std::string prefix;
#endif
    const std::string command =
        prefix + quoted(cli) + " " + arguments + " > " + quoted(log) + " 2>&1";
    const int status = std::system(command.c_str());
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string region_body(const std::string& text, std::string_view region_id) {
    const std::string start = "// [DEVELOPER_LOGIC_START: " + std::string(region_id) + "]";
    const std::string end = "// [DEVELOPER_LOGIC_END: " + std::string(region_id) + "]";
    const std::size_t begin = text.find(start);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t body = begin + start.size();
    const std::size_t finish = text.find(end, body);
    if (finish == std::string::npos) {
        return {};
    }
    return text.substr(body, finish - body);
}

std::string codegen_arguments(const fs::path& out, const fs::path& sample_dir) {
    return "--out " + quoted(out) + " --namespace demo " + quoted(sample_dir / "Demo.tsx");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: codegen_cli_tests SAMPLE_DIR SEED_DIR CLI\n";
        return 2;
    }
    const fs::path sample_dir(argv[1]);
    const fs::path seed_dir(argv[2]);
    const fs::path cli = fs::path(argv[3]).make_preferred();
    const fs::path work = fs::temp_directory_path() / "tessera-codegen-cli-test";
    const fs::path log_dir = work / "logs";
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(log_dir);
    const fs::path out = work / "out";
    fs::create_directories(out);
    fs::create_directories(work / "outside");

    const std::string committed_seed = read_file(seed_dir / "Demo.cpp");
    check(!committed_seed.empty(), "the committed developer seed is readable");
    write_file(out / "Demo.cpp", committed_seed);

    // First run: writes the regenerated files, preserving every developer region.
    check(run_cli(cli, "--write " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "the first --write exits 0");
    const std::string first_cpp = read_file(out / "Demo.cpp");
    check(fs::exists(out / "Demo.hpp"), "the first run writes the header");
    check(first_cpp == committed_seed,
          "regeneration reproduces the committed developer file byte-for-byte");
    check(region_body(first_cpp, "bind::sync").find("state_count_ < 0") != std::string::npos,
          "the sync developer body is preserved");
    check(region_body(first_cpp, "bind::sync_cleanup").find("Hand-written C++") !=
              std::string::npos,
          "the cleanup developer body is preserved");
    check(region_body(first_cpp, "bind::on_name").empty(),
          "a removed binding drops its developer region");
    check(region_body(first_cpp, "bind::sync").find("state_count_ < 0") != std::string::npos,
          "the sync developer body is preserved");

    const auto first_time = fs::last_write_time(out / "Demo.cpp");
    check(run_cli(cli, "--write " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "the second --write exits 0");
    check(read_file(out / "Demo.cpp") == first_cpp, "the second run is byte-identical");
    check(fs::last_write_time(out / "Demo.cpp") == first_time,
          "the second run leaves mtime untouched (no-op regeneration)");
    check(run_cli(cli, "--verbose --write " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "a verbose no-op run exits 0");
    check(read_file(log_dir / "cli.log").find("unchanged") != std::string::npos,
          "the verbose no-op run reports the file as unchanged");

    // A clean tree passes --check.
    check(run_cli(cli, "--check " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "--check is 0 when the output is current");

    // An outside-region edit is drift for --check and is overwritten by --write.
    std::string edited = first_cpp;
    edited += "\n// outside-region manual edit\n";
    write_file(out / "Demo.cpp", edited);
    check(run_cli(cli, "--check " + codegen_arguments(out, sample_dir), log_dir) == 1,
          "--check reports drift outside protected regions");
    check(run_cli(cli, "--write " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "--write repairs the drift");
    const std::string repaired = read_file(out / "Demo.cpp");
    check(repaired.find("outside-region manual edit") == std::string::npos,
          "outside-region edits are overwritten");
    check(region_body(repaired, "bind::sync").find("state_count_ < 0") != std::string::npos,
          "the developer region survives the outside-region repair");

    // Nothing outside the output directory is touched.
    write_file(work / "outside" / "sentinel.txt", "keep");
    check(run_cli(cli, "--write " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "another run exits 0");
    check(read_file(work / "outside" / "sentinel.txt") == "keep",
          "files outside the output directory are untouched");

    // Diagnostics: a broken tsx exits 1 with a file:line diagnostic.
    const fs::path broken = work / "broken.tsx";
    write_file(broken, "export function Broken() {\n  return <View>;\n}\n");
    check(run_cli(cli, "--write --out " + quoted(work / "broken-out") + " " + quoted(broken),
                  log_dir) == 1,
          "a parse error exits 1");
    const std::string diagnostic = read_file(log_dir / "cli.log");
    check(diagnostic.find("broken.tsx:") != std::string::npos,
          "the diagnostic carries the file and line");
    check(!fs::exists(work / "broken-out" / "Broken.cpp"),
          "a diagnostic produces no partial output");

    // --salt assigns numeric automation ids; they depend on the salt and the project-relative path,
    // not on where the project is checked out.
    const fs::path project_a = work / "project-a";
    const fs::path project_b = work / "deep" / "project-b";
    fs::create_directories(project_a);
    fs::create_directories(project_b);
    fs::copy_file(sample_dir / "Demo.tsx", project_a / "Demo.tsx");
    fs::copy_file(sample_dir / "Demo.tsx", project_b / "Demo.tsx");
    const auto salted = [&](const fs::path& project, const fs::path& output, const char* salt) {
        return run_cli(cli,
                       "--write --out " + quoted(output) + " --namespace demo --salt " + salt +
                           " --root " + quoted(project) + " " + quoted(project / "Demo.tsx"),
                       log_dir);
    };
    check(salted(project_a, work / "salt-a", "factory") == 0, "a salted run exits 0");
    check(salted(project_b, work / "salt-b", "factory") == 0, "a salted run elsewhere exits 0");
    check(salted(project_a, work / "salt-c", "other") == 0,
          "a salted run with another salt exits 0");
    const std::string ids_a = read_file(work / "salt-a" / "Demo.cpp");
    check(ids_a.find("set_automation_id(") != std::string::npos,
          "a salted run gives every node its automation id");
    check(ids_a == read_file(work / "salt-b" / "Demo.cpp"),
          "the ids do not depend on the checkout location");
    check(ids_a != read_file(work / "salt-c" / "Demo.cpp"), "another salt gives other ids");
    check(read_file(out / "Demo.cpp").find("set_automation_id(") == std::string::npos,
          "without a salt no ids are assigned");
    check(run_cli(cli,
                  "--write --out " + quoted(work / "salt-d") + " --salt factory --root " +
                      quoted(project_a) + " " + quoted(project_b / "Demo.tsx"),
                  log_dir) == 2,
          "a salted input outside the root exits 2");

    // The salt defaults to `app.namespace` of the tessera.json found above the input; the project
    // root is that file's directory, so ids match an explicit --salt/--root run.
    const fs::path configured = work / "configured";
    fs::create_directories(configured / "ui");
    fs::copy_file(sample_dir / "Demo.tsx", configured / "ui" / "Demo.tsx");
    write_file(configured / "tessera.json", R"({"schemaVersion":1,"app":{"namespace":"factory"}})");
    const auto from_config = [&](const fs::path& output, const std::string& extra = {}) {
        return run_cli(cli,
                       "--write --out " + quoted(output) + " --namespace demo " + extra +
                           quoted(configured / "ui" / "Demo.tsx"),
                       log_dir);
    };
    check(from_config(work / "cfg-a") == 0, "a run with a tessera.json exits 0");
    check(run_cli(cli,
                  "--write --out " + quoted(work / "cfg-b") + " --namespace demo --salt factory " +
                      "--root " + quoted(configured) + " " + quoted(configured / "ui" / "Demo.tsx"),
                  log_dir) == 0,
          "the explicit equivalent exits 0");
    check(read_file(work / "cfg-a" / "Demo.cpp").find("set_automation_id(") != std::string::npos,
          "the config namespace assigns ids without --salt");
    check(read_file(work / "cfg-a" / "Demo.cpp") == read_file(work / "cfg-b" / "Demo.cpp"),
          "the config gives the same ids as --salt with --root");
    check(from_config(work / "cfg-c", "--salt other ") == 0, "--salt overrides the config");
    check(read_file(work / "cfg-c" / "Demo.cpp") != read_file(work / "cfg-a" / "Demo.cpp"),
          "the overriding salt changes the ids");

    // The automation manifest is written next to the generated files, is identical for the same
    // ids, is a no-op on a second run, and is covered by --check.
    const fs::path manifest = work / "cfg-a" / "automation.manifest.json";
    check(fs::exists(manifest), "a salted run writes automation.manifest.json");
    const std::string manifest_text = read_file(manifest);
    check(manifest_text.find("\"projectSalt\": \"factory\"") != std::string::npos &&
              manifest_text.find("\"path\": \"Demo/root[0]") != std::string::npos,
          "the manifest names the salt and the node paths");
    check(manifest_text == read_file(work / "cfg-b" / "automation.manifest.json"),
          "the manifest is the same for the same ids");
    check(manifest_text != read_file(work / "cfg-c" / "automation.manifest.json"),
          "another salt gives another manifest");
    check(!fs::exists(out / "automation.manifest.json"), "without a salt no manifest is written");
    const auto manifest_time = fs::last_write_time(manifest);
    check(from_config(work / "cfg-a") == 0, "a second salted run exits 0");
    check(fs::last_write_time(manifest) == manifest_time,
          "the second run leaves the manifest untouched");
    check(run_cli(cli,
                  "--check --out " + quoted(work / "cfg-a") + " --namespace demo " +
                      quoted(configured / "ui" / "Demo.tsx"),
                  log_dir) == 0,
          "--check passes on a current manifest");
    write_file(manifest, manifest_text + " ");
    check(run_cli(cli,
                  "--check --out " + quoted(work / "cfg-a") + " --namespace demo " +
                      quoted(configured / "ui" / "Demo.tsx"),
                  log_dir) == 1,
          "--check reports a stale manifest");

    // A tessera.json that is found must be usable: no silent unassigned ids.
    write_file(configured / "tessera.json", R"({"schemaVersion":1,"app":{"namespace":""}})");
    check(from_config(work / "cfg-d") == 1, "an empty app.namespace exits 1");
    check(read_file(log_dir / "cli.log").find("app.namespace") != std::string::npos,
          "the diagnostic names app.namespace");
    write_file(configured / "tessera.json", R"({"schemaVersion":1})");
    check(from_config(work / "cfg-e") == 1, "a missing app.namespace exits 1");
    write_file(configured / "tessera.json", "{not json");
    check(from_config(work / "cfg-f") == 1, "a malformed tessera.json exits 1");
    check(!fs::exists(work / "cfg-f" / "Demo.cpp"), "a bad config produces no output");

    // Usage errors exit 2.
    check(run_cli(cli, "", log_dir) == 2, "no arguments exits 2");
    check(run_cli(cli, "--out " + quoted(work / "no-input-out"), log_dir) == 2,
          "a missing input exits 2");
    check(run_cli(cli, "--help", log_dir) == 0, "--help exits 0");

    fs::remove_all(work, error);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen cli tests passed\n";
    return 0;
}

#else // TESSERA_CLI_TESTS_NO_SUBPROCESS

int main() {
    std::cout
        << "codegen cli tests skipped: subprocess execution is unavailable on this platform\n";
    return 0;
}

#endif // TESSERA_CLI_TESTS_NO_SUBPROCESS
