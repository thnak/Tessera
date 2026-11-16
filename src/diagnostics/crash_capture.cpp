// SPDX-License-Identifier: MIT

// Platform-neutral half of the crash capture seam: decide the report path, remember it, and expose
// it to the handler. The platform arms (`crash_capture_posix.cpp`, `crash_capture_win.cpp`) own the
// fatal dispositions and the write itself; keeping the path arithmetic here means both arms agree
// on the file name and neither has to allocate in the handler.

#include "crash_capture_internal.hpp"
#include "tessera/diagnostics/crash_capture.hpp"

#include <cstring>

namespace tessera::diagnostics {

namespace detail {

CaptureState& capture_state() noexcept {
    // Function-local static: constant-initialized, so reading it from a signal handler touches no
    // guard variable and never runs a lazy constructor.
    static CaptureState state;
    return state;
}

} // namespace detail

namespace {

// Fixed name, not a timestamp: the record must not consult a clock (tests compare bytes), and one
// record per process is the bound the uploader needs. A second crash of the same process overwrites
// the first, which is the documented behavior (`crash-reporting-v1.md` section 6.1 "bounded").
constexpr std::string_view kReportName = "crash-report.txt";
constexpr std::string_view kTempSuffix = ".tmp";

void copy_into(std::string_view text, char* out, std::size_t capacity) noexcept {
    std::size_t count = text.size();
    if (count >= capacity) {
        count = capacity - 1;
    }
    for (std::size_t index = 0; index < count; ++index) {
        out[index] = text[index];
    }
    out[count] = '\0';
}

} // namespace

namespace detail {

CrashCaptureDetails install_capture_paths(const CrashCaptureConfig& config) noexcept {
    CrashCaptureDetails details;
    detail::CaptureState& state = detail::capture_state();
    if (!state.installed) {
        if (config.crash_dir.empty()) {
            return details;
        }
        std::string directory(config.crash_dir);
        while (!directory.empty() && (directory.back() == '/' || directory.back() == '\\')) {
            directory.pop_back();
        }
        if (directory.empty()) {
            return details;
        }
        const std::string separator = directory.find('\\') != std::string::npos ? "\\" : "/";
        const std::string report = directory + separator + std::string(kReportName);
        if (report.size() >= detail::kReportPathCapacity) {
            return details;
        }
        copy_into(report, state.report_path, sizeof(state.report_path));
        copy_into(directory + separator + std::string(kReportName) + std::string(kTempSuffix),
                  state.temp_path, sizeof(state.temp_path));
        state.installed = true;
    }
    // Already installed (a second call): keep the original path so the armed handler and the caller
    // never disagree about where the record lives.
    details.report_path.assign(state.report_path);
    return details;
}

} // namespace detail

std::string_view crash_report_path() noexcept {
    const detail::CaptureState& state = detail::capture_state();
    if (!state.installed) {
        return {};
    }
    return std::string_view(state.report_path);
}

void uninstall_crash_capture() noexcept {
    detail::CaptureState& state = detail::capture_state();
    state.installed = false;
}

} // namespace tessera::diagnostics
