// SPDX-License-Identifier: MIT

// POSIX crash arm: a `sigaction` handler writes a bounded local record with async-signal-safe
// calls only (docs/specs/crash-reporting-v1.md section 6.1).
//
// Async-signal-safe means, concretely, that nothing here calls `malloc`, `free`, `std::string`,
// iostreams, `snprintf`, `syslog`, or anything that takes a lock the crashing thread may already
// hold. What is used: `open`/`write`/`close`/`read`/`lseek` (all AS-safe), `sigaction`,
// `sigaltstack`, `raise`, `_exit`, `backtrace` and fixed-size stack buffers. The module table and
// the metadata strings are captured at install time in normal context and only *read* here.
//
// The handler runs on a reserved alternate stack (`sigaltstack`), because a stack-overflow fault
// has no usable stack left and the default disposition would fault again before writing anything.
//
// Deliberate trade (recorded in the report): this arm does not write a core file or a minidump.
// A minidump needs a dynamic library (DbgHelp's `MiniDumpWriteDump` on Windows; on POSIX the
// kernel core pattern) and section 2 of the spec names Crashpad/WER/Breakpad as the out-of-process
// choice, which is a dependency decision this slice does not take (AGENTS.md). The record it
// writes instead is the same metadata the next launch enriches, plus module-relative frames.

#include "tessera/diagnostics/crash_capture.hpp"

#if !defined(_WIN32)

#include "crash_capture_internal.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#endif

#if defined(__ANDROID__)
// Bionic only has `backtrace` from API 33; the app targets API 24, so the unwinder that ships
// with the NDK (`_Unwind_Backtrace`) walks the stack instead.
#include <unwind.h>
#else
#include <execinfo.h>
#endif

// `sigaction` and `backtrace` come from libc/libgcc; no new dependency. `backtrace` is the glibc
// unwinder and is documented as usable from a signal handler when the fault is not on the stack
// it is unwinding (the case a `sigaltstack` handler creates for itself).
namespace tessera::diagnostics {
namespace {

using detail::CaptureMetadata;
using detail::CaptureModuleTable;
using detail::CaptureMoment;
using detail::CaptureState;
using detail::CaptureText;
using detail::kRecordCapacity;

// The frames the handler captured, kept for the record it composes. Fixed array, no allocation.
constexpr std::size_t kMaxBacktraceFrames = detail::kFrameCapacity;
std::uint64_t g_frames[kMaxBacktraceFrames] = {};
std::size_t g_frame_count = 0;

CaptureMetadata g_metadata;
CaptureModuleTable g_modules;
alignas(64) char g_record[kRecordCapacity] = {};

// The alternate stack is statically reserved so arming needs no allocation either. 64 KiB is the
// documented `MINSIGSTKSZ`-safe size on every supported Linux and the same value Apple's
// `sigaltstack(2)` recommends.
constexpr std::size_t kAltStackBytes = 64U * 1024U;
char g_alt_stack[kAltStackBytes] = {};

// Reads `/proc/self/maps` into a fixed buffer and hands each `start-end` range to the module table
// with the last path component of its file name. Only called at install time: `read` is
// AS-safe, but parsing 48 mappings on every crash would be wasted work.
#if defined(__APPLE__)
// Apple has no /proc; the loaded images come from dyld, each as its Mach-O header address and the
// size of its __TEXT segment (the range a return address inside the image falls into).
void capture_modules(CaptureModuleTable& modules) noexcept {
    const std::uint32_t images = _dyld_image_count();
    for (std::uint32_t image = 0; image < images; ++image) {
        const char* name = _dyld_get_image_name(image);
        const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(image));
        if (name == nullptr || header == nullptr) {
            continue;
        }
        unsigned long text_size = 0;
        if (getsegmentdata(header, "__TEXT", &text_size) == nullptr) {
            continue;
        }
        modules.add(std::string_view(name), reinterpret_cast<std::uintptr_t>(header), text_size);
    }
}
#else
void capture_modules(CaptureModuleTable& modules) noexcept {
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    constexpr std::size_t kMapsCapacity = 64U * 1024U;
    static char maps[kMapsCapacity];
    std::size_t used = 0;
    while (used < kMapsCapacity) {
        const ssize_t got = ::read(fd, maps + used, kMapsCapacity - used);
        if (got <= 0) {
            break;
        }
        used += static_cast<std::size_t>(got);
    }
    (void)::close(fd);

    std::size_t index = 0;
    while (index < used) {
        // One mapping line: "start-end perms offset dev inode path".
        std::size_t line_end = index;
        while (line_end < used && maps[line_end] != '\n') {
            ++line_end;
        }
        std::uint64_t start = 0;
        std::uint64_t stop = 0;
        std::size_t cursor = index;
        while (cursor < line_end) {
            const char byte = maps[cursor];
            if (byte >= '0' && byte <= '9') {
                start = start * 16U + static_cast<std::uint64_t>(byte - '0');
                ++cursor;
                continue;
            }
            if (byte >= 'a' && byte <= 'f') {
                start = start * 16U + static_cast<std::uint64_t>(byte - 'a' + 10);
                ++cursor;
                continue;
            }
            break;
        }
        if (cursor < line_end && maps[cursor] == '-') {
            ++cursor;
            while (cursor < line_end) {
                const char byte = maps[cursor];
                if (byte >= '0' && byte <= '9') {
                    stop = stop * 16U + static_cast<std::uint64_t>(byte - '0');
                    ++cursor;
                    continue;
                }
                if (byte >= 'a' && byte <= 'f') {
                    stop = stop * 16U + static_cast<std::uint64_t>(byte - 'a' + 10);
                    ++cursor;
                    continue;
                }
                break;
            }
        }
        // The path starts at the first '/' after the inode column; a mapping with no path
        // (anonymous, [heap], [stack], vdso) is skipped rather than recorded under a fake name.
        std::size_t path_start = line_end;
        for (std::size_t scan = cursor; scan < line_end; ++scan) {
            if (maps[scan] == '/') {
                path_start = scan;
                break;
            }
        }
        if (stop > start && path_start < line_end) {
            modules.add(std::string_view(maps + path_start, line_end - path_start), start,
                        stop - start);
        }
        index = line_end + 1;
    }
}
#endif // __APPLE__

void write_record(const char* path, const CaptureMoment& moment) noexcept {
    const std::size_t size =
        detail::compose_crash_record(moment, g_metadata, g_modules, g_record, kRecordCapacity);
    if (size == 0) {
        return;
    }
    // O_CREAT|O_TRUNC, mode 0600: a crash record is user data, never world-readable.
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return;
    }
    std::size_t written = 0;
    while (written < size) {
        const ssize_t part = ::write(fd, g_record + written, size - written);
        if (part <= 0) {
            break;
        }
        written += static_cast<std::size_t>(part);
    }
    (void)::close(fd);
}

// `_exit` and not `exit`: a corrupted process must not run atexit handlers, flush stdio or take
// the loader lock, any of which can hang forever in a process that just faulted.
[[noreturn]] void terminate_after_write() noexcept {
    (void)::raise(SIGKILL);
    ::_exit(128 + SIGKILL);
}

#if defined(__ANDROID__)
struct UnwindCursor {
    void** pointers;
    int depth;
};

_Unwind_Reason_Code collect_frame(_Unwind_Context* context, void* argument) {
    auto* cursor = static_cast<UnwindCursor*>(argument);
    if (cursor->depth >= static_cast<int>(kMaxBacktraceFrames)) {
        return _URC_END_OF_STACK;
    }
    cursor->pointers[cursor->depth] = reinterpret_cast<void*>(_Unwind_GetIP(context));
    ++cursor->depth;
    return _URC_NO_REASON;
}

int walk_stack(void** pointers) noexcept {
    UnwindCursor cursor{pointers, 0};
    (void)_Unwind_Backtrace(&collect_frame, &cursor);
    return cursor.depth;
}
#else
int walk_stack(void** pointers) noexcept {
    return ::backtrace(pointers, static_cast<int>(kMaxBacktraceFrames));
}

// glibc's `backtrace` is not async-signal-safe on first use: it dlopens `libgcc_s` to unwind, and
// that path allocates. Calling it from the handler would therefore malloc inside a signal, which
// TSan reports as "signal-unsafe call inside of a signal" and which can deadlock for real if the
// fault happened while the heap lock was held. One call at install time pays that cost while the
// process is healthy, so the handler's own call finds everything already resolved.
void warm_backtrace() noexcept {
    void* pointers[kMaxBacktraceFrames] = {};
    (void)::backtrace(pointers, static_cast<int>(kMaxBacktraceFrames));
}
#endif

void capture_frames() noexcept {
    g_frame_count = 0;
    void* pointers[kMaxBacktraceFrames] = {};
    const int depth = walk_stack(pointers);
    for (int index = 0; index < depth; ++index) {
        g_frames[g_frame_count] = reinterpret_cast<std::uint64_t>(pointers[index]);
        ++g_frame_count;
    }
}

extern "C" void crash_signal_handler(int signal_number, siginfo_t* info, void* /*context*/) {
    CaptureState& state = detail::capture_state();
    CaptureMoment moment;
    CaptureText reason;
    switch (signal_number) {
    case SIGSEGV:
        reason.set(std::string_view("access_violation"));
        break;
    case SIGBUS:
        reason.set(std::string_view("bus_error"));
        break;
    case SIGFPE:
        reason.set(std::string_view("arithmetic_fault"));
        break;
    case SIGILL:
        reason.set(std::string_view("illegal_instruction"));
        break;
    case SIGABRT:
        reason.set(std::string_view("abort"));
        break;
    default:
        reason.set(std::string_view("signal"));
        break;
    }
    moment.reason = reason;
    moment.code = static_cast<std::uint64_t>(signal_number);
    CaptureText thread;
    thread.set(std::string_view("main"));
    moment.thread = thread;
    moment.address =
        (info != nullptr && signal_number != SIGABRT)
            ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info->si_addr))
            : 0U;
    // The backtrace call itself allocates inside glibc on some versions, which is why frames are
    // best-effort: if the heap is the thing that died, the record is still written without them.
    capture_frames();
    moment.frames = g_frames;
    moment.frame_count = g_frame_count;
    write_record(state.report_path, moment);
    terminate_after_write();
}

} // namespace

CrashCaptureDetails install_crash_capture(const CrashCaptureConfig& config) {
    CrashCaptureDetails details = detail::install_capture_paths(config);
    detail::CaptureState& state = detail::capture_state();
    if (!state.installed) {
        // Capture is off (no crash directory): nothing is armed, so the process keeps the default
        // dispositions and a crash produces no record.
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
    capture_modules(g_modules);

#if !defined(__ANDROID__)
    // Resolve the unwinder now, not in the handler; see warm_backtrace().
    warm_backtrace();
#endif

    // The alternate stack is what lets a stack-overflow fault produce a record at all.
    stack_t alt{};
    alt.ss_sp = g_alt_stack;
    alt.ss_size = kAltStackBytes;
    alt.ss_flags = 0;
    const bool alt_ok = ::sigaltstack(&alt, nullptr) == 0;

    struct sigaction action{};
    action.sa_sigaction = &crash_signal_handler;
    // SA_SIGINFO for `si_addr`, SA_ONSTACK to use the reserved stack, SA_RESETHAND so a fault
    // *inside* the handler re-raises with the default disposition instead of looping forever.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    // Unqualified on purpose: Apple's <signal.h> makes `sigemptyset` a function-like macro, and
    // `::sigemptyset(...)` expands to `::(*(...) = 0, 0)`, which does not parse.
    sigemptyset(&action.sa_mask);
    details.alt_stack_ready = alt_ok;
    for (const int signal_number : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        if (::sigaction(signal_number, &action, nullptr) == 0) {
            details.armed = true;
        }
    }
    return details;
}

} // namespace tessera::diagnostics

#endif // !defined(_WIN32)
