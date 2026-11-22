// SPDX-License-Identifier: MIT
//
// `tessera.json` `permissions` (issue #451, docs/specs/permissions-v1.md section 7): the declared
// ids are validated against the closed set, and a typo names the valid ones instead of becoming a
// permission the app can never request.
//
// Its own translation unit rather than a block appended to `codegen_cli_tests.cpp`, which is at its
// file-size budget: the gate is what caught that, and the rule it enforces ("split by
// responsibility") is also the right one here, because this is about one config key.

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
    // status 0) as the same, so every rejection case "passed" while asserting nothing - which is the
    // failure mode a cross-platform lane exists to find.
    if (status == -1) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: permissions_config_tests <codegen-cli>\n";
        return 2;
    }
    const std::string cli = argv[1];

    std::error_code ignored;
    // A work directory under the build tree's parent is not available here (the test gets no such
    // argument), so the temp dir is used - but `std::system` on Windows cannot be trusted with a
    // path that contains a space, and `%TEMP%` under a user profile often does ("C:\Users\First
    // Last"). A short name in the current directory is space-free by construction, and the test
    // removes it at the end.
    const fs::path work = fs::current_path(ignored) / "permission-config-scratch";
    fs::remove_all(work, ignored);
    fs::create_directories(work);
    const fs::path log_dir = work / "logs";
    fs::create_directories(log_dir);

    // A minimal project: one component and a config that names it.
    const fs::path project = work / "project";
    write_file(project / "ui" / "App.tsx", "function App() { return <View />; }\n");
    const auto run = [&](const fs::path& output) {
        std::string arguments = "--write --out ";
        arguments += shell_quote(output.generic_string());
        arguments += " --root ";
        arguments += shell_quote(project.generic_string());
        arguments += ' ';
        arguments += shell_quote((project / "ui" / "App.tsx").generic_string());
        return run_cli(cli, arguments, log_dir);
    };
    const auto write_config = [&](std::string_view permissions) {
        write_file(project / "tessera.json",
                   std::string(R"({"schemaVersion":1,"app":{"namespace":"factory"})") +
                       std::string(permissions) + "}");
    };

    // Two valid ids are accepted.
    write_config(R"(,"permissions":["camera","microphone"])");
    check(run(work / "perm-a") == 0, "two valid declared permissions are accepted");

    // A duplicate is a no-op: declaring `camera` twice is the same intent, and failing a build over
    // it would be hostile.
    write_config(R"(,"permissions":["camera","camera"])");
    check(run(work / "perm-b") == 0, "a duplicate permission is a no-op");

    // The whole set round-trips.
    write_config(
        R"(,"permissions":["camera","microphone","location","notifications","photo-library",)"
        R"("file-access"])");
    check(run(work / "perm-all") == 0, "every id in the closed set is accepted");

    // An id outside the set is an error, and the diagnostic names it and lists the valid ones.
    write_config(R"(,"permissions":["camra"])");
    check(run(work / "perm-c") == 1, "an unknown permission id is rejected");
    const std::string log = read_file(log_dir / "cli.log");
    check(log.find("`permissions` entry `camra` is not a permission") != std::string::npos,
          "the diagnostic names the offending id");
    check(log.find("photo-library") != std::string::npos, "and lists the valid set");

    // The ids are case-sensitive: they are the config vocabulary, not a display string.
    write_config(R"(,"permissions":["Camera"])");
    check(run(work / "perm-d") == 1, "the ids are case-sensitive");

    // A non-array is rejected rather than silently ignored, which is the rule every closed key here
    // follows.
    write_config(R"(,"permissions":"camera")");
    check(run(work / "perm-e") == 1, "a non-array `permissions` is rejected");

    // A non-string entry is rejected for the same reason.
    write_config(R"(,"permissions":[7])");
    check(run(work / "perm-f") == 1, "a non-string permission entry is rejected");

    // An absent key is legal: nothing declared means nothing requestable, not a config error.
    write_file(project / "tessera.json", R"({"schemaVersion":1,"app":{"namespace":"factory"}})");
    check(run(work / "perm-none") == 0, "an absent `permissions` key is accepted");

    if (failures != 0) {
        std::cerr << "permissions_config: " << failures << " failure(s)\n";
        return 1;
    }
    fs::remove_all(work, ignored);
    std::cout << "permissions_config: ok\n";
    return 0;
}
