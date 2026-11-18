// SPDX-License-Identifier: MIT

// Codegen CLI and regeneration (Big Step C, ADR-040): run the real executable twice over the
// committed sample screen, assert the second run is a byte/mtime no-op, the generated base is
// rewritten wholesale while the developer files are scaffolded once and never edited, --check
// reports drift, and exit codes are the documented 0/1/2.

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

std::string codegen_arguments(const fs::path& out, const fs::path& sample_dir) {
    return "--out " + quoted(out) + " --namespace demo " + quoted(sample_dir / "Demo.tsx");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: codegen_cli_tests SAMPLE_DIR CLI\n";
        return 2;
    }
    const fs::path sample_dir(argv[1]);
    const fs::path cli = fs::path(argv[2]).make_preferred();
    const fs::path work = fs::temp_directory_path() / "tessera-codegen-cli-test";
    const fs::path log_dir = work / "logs";
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(log_dir);
    const fs::path out = work / "out";
    fs::create_directories(out);
    fs::create_directories(work / "outside");

    // The committed developer class (samples/compiler-demo/Demo.cpp) is the developer-owned half of
    // the demo: it derives from the generated base and defines every `bind::` member. It is the
    // file the CLI must never overwrite.
    const std::string committed_developer = read_file(sample_dir / "Demo.cpp");
    check(!committed_developer.empty(), "the committed developer class is readable");
    check(committed_developer.find("DEVELOPER_LOGIC") == std::string::npos,
          "the committed developer class is a plain file, not a protected-region output");
    check(committed_developer.find("Demo::sync()") != std::string::npos,
          "the committed developer class defines the bound member");

    // First run: writes the generated base and scaffolds the developer files.
    const auto first_run = run_cli(cli, "--write " + codegen_arguments(out, sample_dir), log_dir);
    check(first_run == 0, "the first --write exits 0");
    check(fs::exists(out / "Demo.g.hpp"), "the first run writes the generated base");
    const std::string first_header = read_file(out / "Demo.g.hpp");
    check(first_header.find("gen::DemoBase<Self>") != std::string::npos ||
              first_header.find("struct DemoBase {") != std::string::npos,
          "the generated base declares the CRTP template");
    check(first_header.find("DEVELOPER_LOGIC") == std::string::npos,
          "the generated base carries no protected regions");

    // The developer class is scaffolded with one member per `bind::` id and derives from the base.
    const std::string stub_header = read_file(out / "Demo.hpp");
    check(stub_header.find("class Demo final : public gen::DemoBase<Demo>") != std::string::npos,
          "the developer class derives from the generated base");
    check(stub_header.find("// bind::sync") != std::string::npos,
          "the stub carries a marker per binding");
    const std::string stub_source = read_file(out / "Demo.cpp");
    check(stub_source.find("Demo::sync()") != std::string::npos,
          "the scaffold defines a body per binding");

    // The generated base is a byte/mtime no-op on a second run; the developer files are not edited.
    const auto header_time = fs::last_write_time(out / "Demo.g.hpp");
    const auto stub_time = fs::last_write_time(out / "Demo.cpp");
    check(run_cli(cli, "--verbose --write " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "the second --write exits 0");
    check(read_file(out / "Demo.g.hpp") == first_header, "the second run is byte-identical");
    check(fs::last_write_time(out / "Demo.g.hpp") == header_time,
          "the second run leaves the generated header mtime untouched");
    check(fs::last_write_time(out / "Demo.cpp") == stub_time,
          "the second run leaves the developer file mtime untouched");
    check(read_file(log_dir / "cli.log").find("unchanged") != std::string::npos,
          "the verbose no-op run reports the file as unchanged");

    // A developer edit survives a rerun: the scaffold is written once, never rewritten.
    write_file(out / "Demo.cpp", read_file(out / "Demo.cpp") + "\n// mine\n");
    check(run_cli(cli, "--write " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "a rerun over an edited developer file exits 0");
    check(read_file(out / "Demo.cpp").find("// mine") != std::string::npos,
          "an existing developer file is never overwritten");

    // A clean tree passes --check.
    check(run_cli(cli, "--check " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "--check is 0 when the output is current");

    // A generated-base edit is drift for --check and is overwritten by --write.
    write_file(out / "Demo.g.hpp", first_header + "\n// drift\n");
    check(run_cli(cli, "--check " + codegen_arguments(out, sample_dir), log_dir) == 1,
          "--check reports drift of the generated base");
    check(run_cli(cli, "--write " + codegen_arguments(out, sample_dir), log_dir) == 0,
          "--write repairs the drift");
    check(read_file(out / "Demo.g.hpp").find("// drift") == std::string::npos,
          "the generated base is overwritten wholesale");
    check(read_file(out / "Demo.cpp").find("// mine") != std::string::npos,
          "the developer file is still untouched after a drift repair");

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
    const std::string ids_a = read_file(work / "salt-a" / "Demo.g.hpp");
    check(ids_a.find("set_automation_id(") != std::string::npos,
          "a salted run gives every node its automation id");
    check(ids_a == read_file(work / "salt-b" / "Demo.g.hpp"),
          "the ids do not depend on the checkout location");
    check(ids_a != read_file(work / "salt-c" / "Demo.g.hpp"), "another salt gives other ids");
    check(read_file(out / "Demo.g.hpp").find("set_automation_id(") == std::string::npos,
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
    check(read_file(work / "cfg-a" / "Demo.g.hpp").find("set_automation_id(") != std::string::npos,
          "the config namespace assigns ids without --salt");
    check(read_file(work / "cfg-a" / "Demo.g.hpp") == read_file(work / "cfg-b" / "Demo.g.hpp"),
          "the config gives the same ids as --salt with --root");
    check(from_config(work / "cfg-c", "--salt other ") == 0, "--salt overrides the config");
    check(read_file(work / "cfg-c" / "Demo.g.hpp") != read_file(work / "cfg-a" / "Demo.g.hpp"),
          "the overriding salt changes the ids");

    // Package roots (project-config-v1.md `packages`, issue #70): a bare specifier resolves inside
    // the configured root, and the driver follows the import, so the package source does not have
    // to be listed on the command line.
    const fs::path packaged = work / "packaged";
    fs::create_directories(packaged / "ui");
    fs::create_directories(packaged / "packages" / "tessera-ui");
    write_file(packaged / "ui" / "App.tsx", "import { Card } from \"tessera-ui/Card\";\n"
                                            "function App() { return <Card />; }\n");
    write_file(packaged / "packages" / "tessera-ui" / "Card.tsx",
               "function Card() { return <View />; }\n");
    const auto run_packaged = [&](const fs::path& output) {
        return run_cli(cli,
                       "--write --out " + quoted(output) + " --namespace demo " +
                           quoted(packaged / "ui" / "App.tsx"),
                       log_dir);
    };
    const std::string packages =
        R"("packages":[{"name":"tessera-ui","root":"packages/tessera-ui"}])";
    write_file(packaged / "tessera.json",
               R"({"schemaVersion":1,"app":{"namespace":"factory"},)" + packages + "}");
    check(run_packaged(work / "pkg-a") == 0,
          "a bare specifier resolves through the configured package root");
    bool card_generated = false;
    std::error_code list_error;
    for (const auto& entry : fs::directory_iterator(work / "pkg-a", list_error)) {
        if (entry.path().filename().string().find("Card") != std::string::npos) {
            card_generated = true;
        }
    }
    check(card_generated, "the package component is generated by the same pipeline");

    const std::string duplicate_packages =
        R"("packages":[{"name":"tessera-ui","root":"packages/tessera-ui"},)"
        R"({"name":"tessera-ui","root":"packages/other"}])";
    write_file(packaged / "tessera.json",
               R"({"schemaVersion":1,"app":{"namespace":"factory"},)" + duplicate_packages + "}");
    check(run_packaged(work / "pkg-b") == 1, "a duplicate package name is rejected");

    const std::string absolute_packages =
        R"("packages":[{"name":"tessera-ui","root":"/abs/tessera-ui"}])";
    write_file(packaged / "tessera.json",
               R"({"schemaVersion":1,"app":{"namespace":"factory"},)" + absolute_packages + "}");
    check(run_packaged(work / "pkg-c") == 1, "an absolute package root is rejected");

    write_file(packaged / "tessera.json", R"({"schemaVersion":1,"app":{"namespace":"factory"}})");
    write_file(packaged / "ui" / "App.tsx", "import { Card } from \"missing-ui/Card\";\n"
                                            "function App() { return <Card />; }\n");
    check(run_packaged(work / "pkg-d") == 1, "an unconfigured package is rejected");
    check(read_file(log_dir / "cli.log").find("neither a project file") != std::string::npos,
          "the unknown-package diagnostic names the specifier's problem");

    // Package versions (issue #234, project-config-v1.md `packages`): a declared `version` is
    // checked against the package root's tessera-package.json before anything is compiled.
    write_file(packaged / "packages" / "tessera-ui" / "primitives.tsx",
               "function Pressable() { return <View />; }\n");
    write_file(packaged / "ui" / "App.tsx",
               "import { Card } from \"tessera-ui/Card\";\n"
               "import { Pressable } from \"tessera-ui/primitives\";\n"
               "function App() { return <View><Card /><Pressable /></View>; }\n");
    const fs::path manifest_file = packaged / "packages" / "tessera-ui" / "tessera-package.json";
    int version_case = 0;
    const auto run_versioned = [&](const std::string& required, const std::string& manifest_text) {
        const std::string version =
            required.empty() ? std::string() : ",\"version\":\"" + required + "\"";
        write_file(packaged / "tessera.json",
                   R"({"schemaVersion":1,"app":{"namespace":"factory"},)"
                   R"("packages":[{"name":"tessera-ui","root":"packages/tessera-ui")" +
                       version + "}]}");
        std::error_code ignored;
        fs::remove(manifest_file, ignored);
        if (!manifest_text.empty()) {
            write_file(manifest_file, manifest_text);
        }
        return run_cli(cli,
                       "--write --out " + quoted(work / ("ver-" + std::to_string(++version_case))) +
                           " --namespace demo " + quoted(packaged / "ui" / "App.tsx"),
                       log_dir);
    };
    const auto cli_says = [&](std::string_view text) {
        return read_file(log_dir / "cli.log").find(text) != std::string::npos;
    };
    const auto manifest_json = [](std::string_view version) {
        return "{\"name\":\"tessera-ui\",\"version\":\"" + std::string(version) + "\"}";
    };
    check(run_versioned({}, {}) == 0, "a package without a version needs no manifest");
    check(run_versioned("^1.2.0", manifest_json("1.4.0")) == 0,
          "a caret range accepts a newer minor");
    check(fs::exists(work / "ver-2" / "Pressable.cpp"),
          "the `tessera-ui/primitives` subpath is compiled with the package");
    check(run_versioned("1.2.0", manifest_json("1.2.0")) == 0, "an exact version accepts itself");
    check(run_versioned("1.2.0", manifest_json("1.2.1")) == 1 &&
              cli_says("TESSERA_PACKAGE_VERSION"),
          "an exact version rejects a different patch");
    check(run_versioned("^1.2.0", manifest_json("2.0.0")) == 1 &&
              cli_says("TESSERA_PACKAGE_VERSION"),
          "a caret range rejects a different major");
    check(run_versioned("^1.2.3", manifest_json("1.2.2")) == 1 &&
              cli_says("TESSERA_PACKAGE_VERSION"),
          "a caret range rejects an older patch");
    check(run_versioned("^1.2.0", {}) == 1 && cli_says("TESSERA_PACKAGE_MANIFEST"),
          "a declared version without a manifest is rejected");
    check(run_versioned("^1.2.0", "{\"name\":\"other\",\"version\":\"1.2.0\"}") == 1 &&
              cli_says("TESSERA_PACKAGE_MANIFEST"),
          "a manifest naming another package is rejected");
    check(run_versioned("latest", manifest_json("1.2.0")) == 1 && cli_says("X.Y.Z"),
          "a requirement that is not X.Y.Z or ^X.Y.Z is rejected");

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

    // Developer-file contract (ADR-040 decision item 6, issue #209): a binding the developer class
    // does not define is a warning with its code, not a failure, and a stub marker whose binding
    // left the TSX is reported unused.
    {
        const fs::path dev_out = work / "dev-out";
        fs::create_directories(dev_out);
        const std::string dev_args = codegen_arguments(dev_out, sample_dir);
        check(run_cli(cli, "--write " + dev_args, log_dir) == 0, "dev: the first --write exits 0");

        // Rename the `sync_cleanup` declaration AND definition so the developer class no longer
        // defines the binding (the check reads the two files together).
        for (const char* file : {"Demo.hpp", "Demo.cpp"}) {
            const fs::path path = dev_out / file;
            std::string text = read_file(path);
            const std::size_t at = text.find("sync_cleanup(");
            check(at != std::string::npos, std::string("dev: ") + file + " names sync_cleanup");
            if (at == std::string::npos) {
                continue;
            }
            text.replace(at, 13, "gone_cleanup(");
            write_file(path, text);
        }
        check(run_cli(cli, "--write " + dev_args, log_dir) == 0, "dev: a missing binding exits 0");
        check(read_file(log_dir / "cli.log").find("TESSERA_CG_MISSING_BINDING") !=
                  std::string::npos,
              "dev: a missing binding is reported with its code");

        // A stub marker for a binding that is not in the TSX is reported unused.
        std::string stale = read_file(dev_out / "Demo.hpp");
        const std::string marker = "    // bind::sync_cleanup";
        const std::size_t at = stale.find(marker);
        check(at != std::string::npos, "dev: the stub carries the sync_cleanup marker");
        if (at != std::string::npos) {
            stale.replace(at, marker.size(), "    // bind::on_removed");
            write_file(dev_out / "Demo.hpp", stale);
            check(run_cli(cli, "--write " + dev_args, log_dir) == 0,
                  "dev: a stale binding marker exits 0");
            check(read_file(log_dir / "cli.log").find("TESSERA_CG_BINDING_UNUSED") !=
                      std::string::npos,
                  "dev: a marker whose binding left the TSX is reported unused");
        }
    }

    // `ui.limits.*` (project-config-v1.md, issue #405): the front end limits come from the config.
    const fs::path limited = work / "limited";
    fs::create_directories(limited);
    write_file(limited / "App.tsx", "function App() { return <View />; }\n");
    const auto run_limited = [&](const std::string& limits, const char* out) {
        write_file(limited / "tessera.json",
                   R"({"schemaVersion":1,"app":{"namespace":"lim"},"ui":{"limits":)" + limits +
                       "}}");
        return run_cli(cli,
                       "--write --out " + quoted(work / out) + " --namespace demo " +
                           quoted(limited / "App.tsx"),
                       log_dir);
    };
    check(run_limited(R"({"maxFileBytes":4194304})", "lim-a") == 0, "a raised limit compiles");
    check(run_limited(R"({"maxFileBytes":8})", "lim-b") != 0, "a lowered limit is enforced");
    check(read_file(log_dir / "cli.log").find("TESSERA_JSX_LIMIT_FILE_SIZE") != std::string::npos,
          "the lowered limit reports the file-size code");
    check(run_limited(R"({"maxTokens":0})", "lim-c") == 1, "a zero limit is rejected");
    check(run_limited(R"({"maxBytes":10})", "lim-d") == 1, "an unknown limit key is rejected");

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
