// SPDX-License-Identifier: MIT

// Forced-crash capture (issue #192, acceptance: "a forced crash writes a local report").
//
// This is a *real* crash in a *real* child process: the helper arms the handler and then faults
// (null write or abort). The parent asserts the child died by the fault, that the report exists,
// and that it carries the fields the handler captured. Nothing here simulates a crash with a return
// code - the child's exit status is the proof that it died.
//
// Platform matrix (stated here because a test that only runs on one arm must say so):
//   * POSIX:    fork/exec + SIGSEGV or SIGABRT. Runs on Linux and macOS.
//   * Windows:  CreateProcess + SEH filter. The child exits with the exception's NTSTATUS.
//   * iOS/tvOS/watchOS: skipped with the printed reason ": skipped" - no forked child, no
//     `CreateProcess`; the capture CMake target is not registered there at all.

#include "tessera/diagnostics/crash_capture.hpp"
#include "tessera/diagnostics/crash_report.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

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

struct ChildResult {
    bool launched = false;
    bool crashed = false;
    int exit_code = 0;
    unsigned long signal_number = 0;
};

} // namespace

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

ChildResult run_crashing_child(const fs::path& helper, const fs::path& crash_dir,
                               std::string_view mode) {
    ChildResult result;
    std::string command = "\"" + helper.string() + "\" \"" + crash_dir.string() + "\" ";
    command.append(mode);
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         nullptr, &startup, &process) == FALSE) {
        return result;
    }
    result.launched = true;
    (void)::WaitForSingleObject(process.hProcess, 60000);
    DWORD exit_code = 0;
    (void)::GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    // A crash terminates with the exception code (0xC0000005 for an access violation,
    // 0xC0000409 for abort via the CRT's fast-fail path): anything in the NTSTATUS range means the
    // process died on a fault rather than returning. The abort arm exits through the CRT's SIGABRT
    // handler with 128+SIGABRT, which is the other crash shape.
    result.crashed = exit_code >= 0x80000000U || exit_code == 134;
    (void)::CloseHandle(process.hThread);
    (void)::CloseHandle(process.hProcess);
    return result;
}

} // namespace

#else

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

ChildResult run_crashing_child(const fs::path& helper, const fs::path& crash_dir,
                               std::string_view mode) {
    ChildResult result;
    const std::string directory = crash_dir.string();
    const std::string mode_text(mode);
    const pid_t child = ::fork();
    if (child < 0) {
        return result;
    }
    if (child == 0) {
        // Child: a fresh image, so the parent's state cannot mask a handler bug.
        ::execl(helper.c_str(), helper.c_str(), directory.c_str(), mode_text.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    result.launched = true;
    int status = 0;
    if (::waitpid(child, &status, 0) != child) {
        return result;
    }
    if (WIFSIGNALED(status)) {
        result.signal_number = static_cast<unsigned long>(WTERMSIG(status));
        result.crashed = true;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
    return result;
}

} // namespace

#endif

namespace {

// Asserts one report against the mode that produced it. Split from the runner so both modes share
// the field checks.
void check_report(const fs::path& report, std::string_view expected_reason,
                  std::string_view label) {
    std::cerr << "  [" << label << "] report: " << report.string() << "\n";
    check(fs::exists(report), "the forced crash wrote a report file");
    if (!fs::exists(report)) {
        return;
    }
    const std::string text = read_file(report);
    std::cerr << "----- report begin -----\n" << text << "----- report end -----\n";
    const auto parsed = tessera::diagnostics::parse_crash_record(text);
    check(parsed.has_value(), "the report parses as a tessera.crash.v1 record");
    if (!parsed.has_value()) {
        std::cerr << "  parse error: " << tessera::diagnostics::to_string(parsed.error()) << "\n";
        return;
    }
    const tessera::diagnostics::CrashRecord& record = *parsed;
    check(!record.truncated, "the report was not truncated");
    check(record.reason == expected_reason, "the crash reason names the fault (" +
                                                std::string(expected_reason) + " vs " +
                                                record.reason + ")");
    // The fields the handler captured from the configuration, byte for byte.
    check(record.app_version == "9.9.9-test", "app version round-trips");
    check(record.commit == "deadbeef", "commit round-trips");
    check(record.build_id == "bld-crash-test", "build id round-trips");
    check(record.platform == "test-platform", "platform round-trips");
    check(record.arch == "test-arch", "arch round-trips");
    check(record.os == "test-os", "os round-trips");
    check(record.gpu == "test-gpu", "gpu round-trips");
    check(record.config_hash == "cfg-123", "config hash round-trips");
    check(!record.thread.empty(), "the report names the crashing thread");
    check(!record.modules.empty(), "the module table was captured");
    // Frames are best-effort (documented trade): a corrupted heap can fail the backtrace, so a
    // missing frame list is reported here rather than asserted. The report must still be usable.
    if (record.frames.empty()) {
        std::cerr << "  note: no frames captured (backtrace unavailable in the fatal path)\n";
    }
    for (const tessera::diagnostics::CrashModule& module : record.modules) {
        check(module.name.find('\\') == std::string::npos &&
                  module.name.find('/') == std::string::npos,
              "module names are basenames, never paths");
        check(module.size != 0, "module sizes are recorded");
    }
    // Privacy: a crash report never carries an absolute user path.
    check(!tessera::diagnostics::contains_unredacted(text),
          "the report contains no unredacted user path or environment value");
}

void run_mode(const fs::path& helper, const fs::path& crash_dir, std::string_view mode,
              std::string_view expected_reason) {
    const fs::path report = crash_dir / "crash-report.txt";
    // The handler does not create the directory (a fatal path must not); the parent does, which is
    // also what a real app does at startup.
    std::error_code error;
    fs::create_directories(crash_dir, error);
    fs::remove(report);
    const ChildResult child = run_crashing_child(helper, crash_dir, mode);
    check(child.launched, "the crashing child process started");
    if (!child.launched) {
        return;
    }
    check(child.crashed, "the child died from the fault, not from a normal return");
    if (!child.crashed) {
        std::cerr << "  exit code: " << child.exit_code << " signal: " << child.signal_number
                  << "\n";
        return;
    }
    check_report(report, expected_reason, mode);
}

} // namespace

int main(int argc, char** argv) {
#if defined(TESSERA_NO_SUBPROCESS)
    // iOS/tvOS/watchOS: no forked child. Printed with the ": skipped" marker the CMake
    // SKIP_REGULAR_EXPRESSION looks for, and the reason is stated rather than the test silently
    // vanishing from the suite.
    std::cout << "crash_capture: skipped - no child process primitive on this platform\n";
    return 0;
#else
    if (argc != 3) {
        std::cerr << "usage: crash_capture_tests HELPER CRASH_DIR\n";
        return 2;
    }
    const fs::path helper(argv[1]);
    const fs::path base(argv[2]);
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base, error);

    std::cerr << "forced crash: null write (access violation / SIGSEGV)\n";
    run_mode(helper, base / "segv", "segv", "access_violation");
    std::cerr << "forced crash: abort\n";
    run_mode(helper, base / "abort", "abort", "abort");

    // The "off" path: an empty crash directory means no handler is armed, so the helper reports
    // that it could not arm instead of crashing unobserved.
    {
        const fs::path report = base / "disabled" / "crash-report.txt";
        std::error_code disabled_error;
        fs::create_directories(base / "disabled", disabled_error);
        const ChildResult child = run_crashing_child(helper, fs::path(), "segv");
        check(child.launched, "the disabled-mode child started");
        check(!child.crashed, "with capture disabled the helper refuses to crash unarmed");
        check(child.exit_code == 3, "the helper reports an unarmed handler as exit code 3");
        check(!fs::exists(report), "with capture disabled no report is written");
    }

    // The install seam itself: an empty crash directory arms nothing and hands back no path.
    {
        tessera::diagnostics::CrashCaptureConfig config;
        const auto details = tessera::diagnostics::install_crash_capture(config);
        check(!details.armed, "an empty crash directory does not arm the handler");
        check(details.report_path.empty(), "an empty crash directory yields no report path");
        check(tessera::diagnostics::crash_report_path().empty(),
              "no report path is published when capture is off");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "crash_capture: ok\n";
    return 0;
#endif
}
