// SPDX-License-Identifier: MIT

// Crash capture: arm the fatal path so a crash writes a bounded local record, and find that record
// again on the next launch (docs/specs/crash-reporting-v1.md section 6.1).
//
// This header is the *handler seam*. The payload shape lives in `crash.hpp`; the platform arms
// (`crash_capture_posix.cpp`, `crash_capture_win.cpp`) fill the same fixed-size record, and
// `crash_report.hpp` reads it back. Everything reachable from the crashing thread is
// allocation-free: the configuration is copied into fixed buffers at install time, and the handler
// only calls async-signal-safe primitives (POSIX) or kernel32 handle calls (Windows).
//
// Deliberate trade, recorded here because it is the point of the design: the record is a small
// text record, not a minidump or a core file. A minidump needs DbgHelp (`MiniDumpWriteDump`) on
// Windows and a core-pattern decision on POSIX, and the spec's out-of-process choice (Crashpad,
// WER, Breakpad) is a dependency decision (AGENTS.md: no new dependency without
// docs/specs/dependencies.md). So this slice writes what it can prove without allocating - version,
// build id, platform, reason, thread, a module table and module-relative frames - and a later
// out-of-process slice can attach a dump next to it.

#pragma once

#include <string>
#include <string_view>

namespace tessera::diagnostics {

struct CrashCaptureConfig {
    // Directory the record is written into. Empty disables capture: no handler is armed and no
    // record is ever written, which is the release-off state.
    std::string_view crash_dir;
    // Copied at install time (the handler must not touch the caller's strings). Empty fields are
    // omitted from the record instead of being written empty.
    std::string_view app_version;
    std::string_view commit;
    std::string_view build_id;
    std::string_view platform;
    std::string_view arch;
    std::string_view os;
    std::string_view gpu;
    std::string_view config_hash;
    // Capture a backtrace at the fault. Off trades an empty `frame` list for the smallest possible
    // handler; see the allocator-safety note in `crash_capture_posix.cpp`.
    bool capture_frames = true;
};

struct CrashCaptureDetails {
    // True when at least one fatal disposition was installed.
    bool armed = false;
    // POSIX only: the alternate signal stack was installed, so a stack-overflow fault can still
    // write a record. Always false on Windows, which uses an exception filter instead.
    bool alt_stack_ready = false;
    // The path the handler will write. Empty when capture is disabled.
    std::string report_path;
};

// Arms the fatal path. Safe to call once per process; a second call returns the current state and
// does not re-arm. Returns an unarmed result when `crash_dir` is empty, so "off" costs nothing.
// Not noexcept: it builds the report path (one std::string) at install time, never in the handler.
[[nodiscard]] CrashCaptureDetails install_crash_capture(const CrashCaptureConfig& config);

// The path armed at install time, or an empty view. Readable by the handler without allocating.
[[nodiscard]] std::string_view crash_report_path() noexcept;

// Restores the default dispositions. For tests that must keep the process alive after a caught
// fault; production never needs it.
void uninstall_crash_capture() noexcept;

} // namespace tessera::diagnostics
