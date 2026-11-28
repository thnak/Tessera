// SPDX-License-Identifier: MIT
//
// `tessera.json` `dependencies` (issue #983, docs/specs/project-config-v1.md): the loader reads and
// validates `includeDirs`, `linkLibraries`, `modules` (the opt-in SQLite module key) and
// `licenseReview`. One config key per executable, like permissions_config_tests.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void write_file(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Wrap a string in double quotes for a shell command line. Named `shell_quote`, not `quoted`:
// `std::quoted` is in <iomanip>/<utility>, unqualified lookup finds it through ADL for a
// `std::string` argument, and the resulting overload set resolves to `_Quote_out` instead of this
// function. Call sites pass `.generic_string()` for a path so nothing implicitly constructs an
// `fs::path`.
[[nodiscard]] std::string shell_quote(std::string_view text) {
    std::string result = "\"";
    result += text;
    result += '"';
    return result;
}

// Runs the codegen CLI over `input` and returns its exit code, with stdout/stderr captured to
// `log_dir/cli.log` so a diagnostic can be asserted.
int run_cli(const std::string& cli, const std::string& arguments, const fs::path& log_dir) {
    std::string command;
#ifdef _WIN32
    // `cmd` strips a leading quote from the whole command line, so a quoted executable path as the
    // first token loses its quotes and becomes "The filename, directory name, or volume label
    // syntax is incorrect". `call` puts a bare token first, exactly as `codegen_cli_tests` does.
    command = "call ";
#endif
    command += shell_quote(cli);
    command += ' ';
    command += arguments;
    command += " > ";
    command += shell_quote((log_dir / "cli.log").generic_string());
    command += " 2>&1";
    const int status = std::system(command.c_str());
#ifdef _WIN32
    return status;
#else
    // POSIX `system` returns a wait status, not an exit code: the code is in the high byte. Without
    // this the test reads a *rejected* run (exit 1 -> status 256) and a successful one (exit 0 ->
    // status 0) as the same, so every rejection case "passed" while asserting nothing - which is
    // the failure mode a cross-platform lane exists to find.
    if (status == -1) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dependencies_config_tests <codegen-cli>\n";
        return 2;
    }
    const std::string cli = argv[1];
    std::error_code ignored;
    const fs::path work = fs::current_path(ignored) / "dependencies-config-scratch";
    fs::remove_all(work, ignored);
    const fs::path log_dir = work / "logs";
    fs::create_directories(log_dir);
    const fs::path project = work / "project";
    write_file(project / "ui" / "App.tsx", "function App() { return <View />; }\n");
    int counter = 0;
    const auto run = [&](std::string_view dependencies) {
        write_file(project / "tessera.json",
                   std::string(R"({"schemaVersion":1,"app":{"namespace":"factory"})") +
                       std::string(dependencies) + "}");
        std::string arguments = "--write --out ";
        arguments += shell_quote((work / ("out-" + std::to_string(counter++))).generic_string());
        arguments += " --root ";
        arguments += shell_quote(project.generic_string());
        arguments += ' ';
        arguments += shell_quote((project / "ui" / "App.tsx").generic_string());
        return run_cli(cli, arguments, log_dir);
    };
    const auto log_has = [&](std::string_view text) {
        return read_file(log_dir / "cli.log").find(text) != std::string::npos;
    };

    check(run("") == 0, "an absent dependencies block is accepted");
    check(run(R"(,"dependencies":null)") == 0, "a null dependencies block is accepted");
    check(run(R"(,"dependencies":{"includeDirs":["vendor/include"],"linkLibraries":["m"],)"
              R"("modules":["sqlite","sqlite"],"licenseReview":true})") == 0,
          "a full dependencies block, with a duplicate module, is accepted");

    check(run(R"(,"dependencies":{"modules":["postgres"]})") == 1, "an unknown module is rejected");
    check(log_has("`dependencies.modules` entry `postgres` is not a module: sqlite"),
          "the diagnostic names the module and the valid set");
    check(run(R"(,"dependencies":{"modules":"sqlite"})") == 1, "a non-array modules is rejected");
    check(run(R"(,"dependencies":{"modules":[3]})") == 1, "a non-string module is rejected");
    check(run(R"(,"dependencies":{"includeDirs":["/abs"]})") == 1,
          "an absolute include dir is rejected");
    check(run(R"(,"dependencies":{"includeDirs":["../up"]})") == 1,
          "an escaping include dir is rejected");
    check(run(R"(,"dependencies":{"licenseReview":false})") == 1,
          "licenseReview false is rejected");
    check(run(R"(,"dependencies":{"extra":1})") == 1, "an unknown dependencies key is rejected");
    check(run(R"(,"dependencies":[])") == 1, "a non-object dependencies is rejected");

    if (failures != 0) {
        std::cerr << "dependencies_config: " << failures << " failure(s)\n";
        return 1;
    }
    fs::remove_all(work, ignored);
    std::cout << "dependencies_config: ok\n";
    return 0;
}
