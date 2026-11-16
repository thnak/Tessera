// SPDX-License-Identifier: MIT

// Windows crash arm: an unhandled-exception filter writes the same bounded local record.
//
// The exception filter runs in a special context: the process is already dead, the heap may be
// corrupt, and the loader lock may be held. So the filter uses only `kernel32` calls that do not
// allocate and do not take the loader lock - `CreateFileW`, `WriteFile`, `CloseHandle`,
// `GetModuleHandleW`-free module bookkeeping done at install time, and `RtlCaptureStackBackTrace`
// over a fixed buffer. No new dependency: `DbgHelp` is deliberately *not* linked here, so no
// `MiniDumpWriteDump` and no symbol handler (see the deliberate-trade note in the header).
//
// SEH is not signals: `SetUnhandledExceptionFilter` catches access violations, illegal
// instructions and `RaiseException`-style aborts, but `std::abort()` in the MSVC runtime raises
// `SIGABRT` after its own filter chain, so this arm additionally arms it through the CRT's
// `signal(SIGABRT, ...)`. Both arms call the same `write_record`, so a report exists either way.

#include "tessera/diagnostics/crash_capture.hpp"

#if defined(_WIN32)

#include "crash_capture_internal.hpp"

#include <csignal>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// psapi.h must follow windows.h: EnumProcessModules and MODULEINFO live there.
#include <psapi.h>
// tlhelp32.h must follow windows.h.
#include <tlhelp32.h>

namespace tessera::diagnostics {
namespace {

using detail::CaptureMetadata;
using detail::CaptureModuleTable;
using detail::CaptureMoment;
using detail::CaptureState;
using detail::CaptureText;
using detail::kRecordCapacity;

constexpr std::size_t kMaxStackFrames = detail::kFrameCapacity;

CaptureMetadata g_metadata;
CaptureModuleTable g_modules;
char g_record[kRecordCapacity] = {};
std::uint64_t g_frames[kMaxStackFrames] = {};
// The module table is captured at install time; the filter only reads it. `g_installed_base_w` is
// kept as a wide string because `CreateFileW` needs one and converting in the filter would
// allocate.
wchar_t g_report_path_w[detail::kReportPathCapacity] = {};
wchar_t g_temp_path_w[detail::kReportPathCapacity] = {};
bool g_capture_frames = true;

// Converts a UTF-8-ish `std::string_view` to the fixed wide buffer. Called at install time only,
// so the `MultiByteToWideChar` call is not on the fatal path.
void to_wide(std::string_view text, wchar_t* out, std::size_t capacity) noexcept {
    if (capacity == 0) {
        return;
    }
    int written = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out,
                                        static_cast<int>(capacity - 1));
    if (written <= 0) {
        written = ::MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), out,
                                        static_cast<int>(capacity - 1));
    }
    out[written > 0 ? written : 0] = L'\0';
}

// Snapshot of the loaded modules, taken at install time. `EnumProcessModules` needs psapi (already
// linked by this library); `GetModuleInformation` gives the real image size, which SEH fault
// addresses are matched against.
void capture_modules(CaptureModuleTable& modules) noexcept {
    HMODULE handles[256] = {};
    DWORD needed = 0;
    if (::EnumProcessModules(::GetCurrentProcess(), handles, sizeof(handles), &needed) == FALSE) {
        return;
    }
    const DWORD count = needed / static_cast<DWORD>(sizeof(HMODULE));
    for (DWORD index = 0; index < count; ++index) {
        MODULEINFO info{};
        wchar_t wide[MAX_PATH] = {};
        if (::GetModuleInformation(::GetCurrentProcess(), handles[index], &info, sizeof(info)) ==
            FALSE) {
            continue;
        }
        const DWORD length = ::GetModuleFileNameW(handles[index], wide, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            continue;
        }
        char narrow[MAX_PATH * 3] = {};
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                  narrow, sizeof(narrow) - 1, nullptr, nullptr);
        if (written <= 0) {
            continue;
        }
        modules.add(std::string_view(narrow, static_cast<std::size_t>(written)),
                    reinterpret_cast<std::uint64_t>(info.lpBaseOfDll), info.SizeOfImage);
    }
}

void write_record(const CaptureMoment& moment) noexcept {
    const std::size_t size =
        detail::compose_crash_record(moment, g_metadata, g_modules, g_record, kRecordCapacity);
    if (size == 0) {
        return;
    }
    HANDLE file = ::CreateFileW(g_temp_path_w, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    std::size_t offset = 0;
    while (offset < size) {
        DWORD part = 0;
        if (::WriteFile(file, g_record + offset, static_cast<DWORD>(size - offset), &part,
                        nullptr) == FALSE) {
            break;
        }
        offset += part;
        written += part;
    }
    (void)::CloseHandle(file);
    if (written != size) {
        return;
    }
    // Publish atomically: a reader never observes a half-written record.
    (void)::MoveFileExW(g_temp_path_w, g_report_path_w,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD* record = pointers != nullptr ? pointers->ExceptionRecord : nullptr;
    CaptureMoment moment;
    CaptureText reason;
    switch (record != nullptr ? record->ExceptionCode : 0U) {
    case EXCEPTION_ACCESS_VIOLATION:
        reason.set(std::string_view("access_violation"));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        reason.set(std::string_view("illegal_instruction"));
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        reason.set(std::string_view("arithmetic_fault"));
        break;
    case EXCEPTION_STACK_OVERFLOW:
        reason.set(std::string_view("stack_overflow"));
        break;
    default:
        reason.set(std::string_view("exception"));
        break;
    }
    moment.reason = reason;
    moment.code = record != nullptr ? static_cast<std::uint64_t>(record->ExceptionCode) : 0U;
    CaptureText thread;
    thread.set(std::string_view("main"));
    moment.thread = thread;
    moment.address = (record != nullptr && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                      record->NumberParameters >= 2)
                         ? static_cast<std::uint64_t>(record->ExceptionInformation[1])
                         : 0U;

    // Capture has to happen here, on the faulting thread: the context and stack are only valid in
    // the filter. `RtlCaptureStackBackTrace` walks the current stack, which is still intact for
    // every exception except a stack overflow (there it returns few or no frames, and the record
    // says so).
    moment.frame_count = 0;
    moment.frames = g_frames;
    if (g_capture_frames && record != nullptr &&
        record->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        const USHORT depth = ::RtlCaptureStackBackTrace(1, static_cast<DWORD>(kMaxStackFrames),
                                                        (void**)g_frames, nullptr);
        moment.frame_count = static_cast<std::size_t>(depth);
    }
    write_record(moment);
    // EXCEPTION_EXECUTE_HANDLER: the filter has done its work, so the process must die rather than
    // resume. Returning CONTINUE_SEARCH here would let the default WER path run and the record
    // would be written twice.
    return EXCEPTION_EXECUTE_HANDLER;
}

void crash_abort_handler(int /*signal_number*/) {
    CaptureMoment moment;
    CaptureText reason;
    reason.set(std::string_view("abort"));
    moment.reason = reason;
    moment.code = 0;
    CaptureText thread;
    thread.set(std::string_view("main"));
    moment.thread = thread;
    moment.frames = g_frames;
    moment.frame_count = 0;
    // The stack is intact for an abort (unlike a stack overflow), so a backtrace is worth taking.
    if (g_capture_frames) {
        const USHORT depth = ::RtlCaptureStackBackTrace(1, static_cast<DWORD>(kMaxStackFrames),
                                                        (void**)g_frames, nullptr);
        moment.frame_count = static_cast<std::size_t>(depth);
    }
    write_record(moment);
    // 134 = 128 + SIGABRT, the conventional abort status. Not 3: the helper uses 3 for "capture
    // was not armed", and the test must be able to tell a written report from an unarmed crash.
    ::_exit(134);
}

} // namespace

CrashCaptureDetails install_crash_capture(const CrashCaptureConfig& config) {
    CrashCaptureDetails details = detail::install_capture_paths(config);
    detail::CaptureState& state = detail::capture_state();
    if (!state.installed) {
        // Capture is off (no crash directory): nothing is armed, so "off" costs one branch and the
        // process keeps the default dispositions.
        return details;
    }
    g_metadata.app_version.set(config.app_version);
    g_metadata.commit.set(config.commit);
    g_metadata.build_id.set(config.build_id);
    g_metadata.platform.set(config.platform);
    g_metadata.arch.set(config.arch);
    g_metadata.os.set(config.os);
    g_metadata.gpu.set(config.gpu);
    g_metadata.config_hash.set(config.config_hash);
    g_capture_frames = config.capture_frames;
    capture_modules(g_modules);

    to_wide(state.report_path, g_report_path_w, detail::kReportPathCapacity);
    to_wide(state.temp_path, g_temp_path_w, detail::kReportPathCapacity);

    ::SetUnhandledExceptionFilter(&crash_exception_filter);
    (void)::signal(SIGABRT, &crash_abort_handler);
    details.armed = true;
    return details;
}

} // namespace tessera::diagnostics

#endif // defined(_WIN32)
