// SPDX-License-Identifier: MIT

// A helper that really crashes, so `crash_capture_tests` can prove the handler writes a report from
// a *separate process* (issue #192, acceptance: "a forced crash writes a local report").
//
// `argv[1]` is the crash directory, `argv[2]` selects the fault:
//   segv  - write through a null pointer (an access violation / SIGSEGV)
//   abort - std::abort (SIGABRT on POSIX, the CRT's abort on Windows)
// This program is expected to die; a zero exit code means the fault did not happen and the test
// treats that as a failure.

#include "tessera/diagnostics/crash_capture.hpp"

#include <cstdlib>
#include <cstring>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    tessera::diagnostics::CrashCaptureConfig config;
    config.crash_dir = argv[1];
    config.app_version = "9.9.9-test";
    config.commit = "deadbeef";
    config.build_id = "bld-crash-test";
    config.platform = "test-platform";
    config.arch = "test-arch";
    config.os = "test-os";
    config.gpu = "test-gpu";
    config.config_hash = "cfg-123";
    const auto details = tessera::diagnostics::install_crash_capture(config);
    if (!details.armed) {
        // The parent asserts the report exists; an unarmed helper must say why it is not going to
        // produce one instead of crashing without a handler.
        return 3;
    }

    const std::string_view mode(argv[2]);
    if (mode == "abort") {
        std::abort();
    }
    // A write through a null pointer is the classic access violation on both arms: SIGSEGV with
    // si_addr == 0 on POSIX, EXCEPTION_ACCESS_VIOLATION with ExceptionInformation[1] == 0 on
    // Windows. `volatile` keeps the compiler from folding it away in Release.
    volatile int* target = nullptr;
    *target = 42;
    // Unreachable: a signal/SH handler has already terminated the process. If it has not, the test
    // must not be told "the crash happened", so exit with a distinct code.
    return 4;
}
