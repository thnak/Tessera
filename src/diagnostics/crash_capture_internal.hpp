// SPDX-License-Identifier: MIT

// Allocation-free writer for the local crash record, shared by the POSIX signal arm and the
// Windows structured-exception arm (docs/specs/crash-reporting-v1.md section 6.1, "minimal
// async-signal-safe write").
//
// The fatal path may not allocate, lock, log or throw: a corrupted heap turns `malloc` into a
// deadlock or a second fault, and a `std::string` needs the very allocator that just died. So
// every helper here is an inline loop over a fixed buffer: no `std::string`, no iostreams, no
// `snprintf` (it takes a locale lock and can allocate on first use). The two arms fill a
// `CaptureMoment` and a `CaptureModuleTable` - both plain structs of fixed arrays - and hand them
// to `compose_crash_record`, so the record format exists once.
//
// The record is deliberately *not* the JSON of `crash.hpp`. JSON escaping of arbitrary text and
// the bounded arrays of `crash_metadata_json` need allocation; the handler writes the raw facts
// (module-relative offsets, no symbols) and the next launch enriches them through `CrashReporter`.

#pragma once

#include "tessera/diagnostics/crash_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::diagnostics::detail {

// Bumped only if the line format changes: the parser refuses a schema line it does not know.
inline constexpr std::string_view kCrashRecordSchema = "tessera.crash.v1";

inline constexpr std::size_t kReportPathCapacity = 512;
inline constexpr std::size_t kModuleNameCapacity = 96;
inline constexpr std::size_t kModuleCapacity = 48;
inline constexpr std::size_t kFrameCapacity = 64;
// The whole record, including a full frame list, is a few KiB; 32 KiB leaves room for 48 module
// names without ever truncating in practice. `compose_crash_record` reports the size it produced
// and stops appending past this bound instead of overflowing.
inline constexpr std::size_t kRecordCapacity = 32U * 1024U;

// State the two arms share with the format-independent half of the module: the paths chosen at
// install time and whether a handler is armed. Written at install (normal context), read by the
// handler and by `crash_report_path()`.
struct CaptureState {
    bool installed = false;
    char report_path[kReportPathCapacity] = {};
    char temp_path[kReportPathCapacity] = {};
};

CaptureState& capture_state() noexcept;

// Chooses `<crash_dir>/crash-report.txt` (plus its `.tmp` staging sibling) and records it in the
// capture state. Returns the details the platform arm extends with what it armed; an unarmed
// result means capture is off. Defined in crash_capture.cpp so both arms share one path rule.
[[nodiscard]] CrashCaptureDetails install_capture_paths(const CrashCaptureConfig& config) noexcept;

// A NUL-terminated, fixed-capacity copy of one metadata field. Copying at install time is what
// makes the handler independent of the caller's string lifetimes.
struct CaptureText {
    char data[192] = {};
    std::size_t size = 0;

    void set(std::string_view value) noexcept {
        std::size_t count = value.size();
        if (count >= sizeof(data)) {
            count = sizeof(data) - 1;
        }
        for (std::size_t index = 0; index < count; ++index) {
            data[index] = value[index];
        }
        data[count] = '\0';
        size = count;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return {data, size};
    }
};

struct CaptureMetadata {
    CaptureText app_version;
    CaptureText commit;
    CaptureText build_id;
    CaptureText platform;
    CaptureText arch;
    CaptureText os;
    CaptureText gpu;
    CaptureText config_hash;
};

// The loaded-module table, captured at install time so the handler can turn an absolute program
// counter into `module + offset` with a loop over fixed arrays. Only the last path component is
// kept (`redact_path`'s rule), so no user directory can reach the record.
struct CaptureModuleTable {
    std::size_t count = 0;
    char names[kModuleCapacity][kModuleNameCapacity] = {};
    std::uint64_t bases[kModuleCapacity] = {};
    std::uint64_t sizes[kModuleCapacity] = {};

    void add(std::string_view path, std::uint64_t base, std::uint64_t size) noexcept {
        if (count >= kModuleCapacity || size == 0) {
            return;
        }
        const std::size_t slash = path.find_last_of("/\\");
        const std::string_view name = (slash == std::string_view::npos || slash + 1 >= path.size())
                                          ? path
                                          : path.substr(slash + 1);
        std::size_t length = name.size();
        if (length >= kModuleNameCapacity) {
            length = kModuleNameCapacity - 1;
        }
        char* slot = names[count];
        for (std::size_t index = 0; index < length; ++index) {
            slot[index] = name[index];
        }
        slot[length] = '\0';
        bases[count] = base;
        sizes[count] = size;
        ++count;
    }

    // The index of the tightest module containing `address`, or -1. Tightest (smallest size) wins
    // so a mapping nested inside another cannot shadow the real owner.
    [[nodiscard]] int index_of(std::uint64_t address) const noexcept {
        int found = -1;
        std::uint64_t found_size = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint64_t start = bases[index];
            const std::uint64_t end = start + sizes[index];
            if (address < start || address >= end) {
                continue;
            }
            if (found < 0 || sizes[index] < found_size) {
                found = static_cast<int>(index);
                found_size = sizes[index];
            }
        }
        return found;
    }
};

// What the handler knew at the moment of the fault. `frames` are absolute program counters;
// they are rewritten as module-relative offsets before anything is written.
struct CaptureMoment {
    CaptureText reason;
    std::uint64_t code = 0;
    CaptureText thread;
    std::uint64_t address = 0;
    const std::uint64_t* frames = nullptr;
    std::size_t frame_count = 0;
};

inline bool append_bytes(char* buffer, std::size_t capacity, std::size_t& size,
                         std::string_view text) noexcept {
    if (size > capacity || text.size() > capacity - size) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        buffer[size + index] = text[index];
    }
    size += text.size();
    return true;
}

inline bool append_decimal(char* buffer, std::size_t capacity, std::size_t& size,
                           std::uint64_t value) noexcept {
    char digits[20] = {};
    std::size_t count = 0;
    do {
        digits[count] = static_cast<char>('0' + static_cast<char>(value % 10U));
        ++count;
        value /= 10U;
    } while (value != 0U);
    if (size > capacity || count > capacity - size) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        buffer[size + index] = digits[count - 1 - index];
    }
    size += count;
    return true;
}

inline bool append_hex(char* buffer, std::size_t capacity, std::size_t& size,
                       std::uint64_t value) noexcept {
    constexpr char kDigits[] = "0123456789abcdef";
    char digits[16] = {};
    std::size_t count = 0;
    do {
        digits[count] = kDigits[value & 0xFU];
        ++count;
        value >>= 4U;
    } while (value != 0U);
    if (size > capacity || count + 2U > capacity - size) {
        return false;
    }
    buffer[size] = '0';
    buffer[size + 1] = 'x';
    size += 2;
    for (std::size_t index = 0; index < count; ++index) {
        buffer[size + index] = digits[count - 1 - index];
    }
    size += count;
    return true;
}

inline bool append_line(char* buffer, std::size_t capacity, std::size_t& size, std::string_view key,
                        std::string_view value) noexcept {
    return append_bytes(buffer, capacity, size, key) && append_bytes(buffer, capacity, size, "=") &&
           append_bytes(buffer, capacity, size, value) &&
           append_bytes(buffer, capacity, size, "\n");
}

inline bool append_uint_line(char* buffer, std::size_t capacity, std::size_t& size,
                             std::string_view key, std::uint64_t value) noexcept {
    return append_bytes(buffer, capacity, size, key) && append_bytes(buffer, capacity, size, "=") &&
           append_decimal(buffer, capacity, size, value) &&
           append_bytes(buffer, capacity, size, "\n");
}

inline bool append_hex_line(char* buffer, std::size_t capacity, std::size_t& size,
                            std::string_view key, std::uint64_t value) noexcept {
    return append_bytes(buffer, capacity, size, key) && append_bytes(buffer, capacity, size, "=") &&
           append_hex(buffer, capacity, size, value) && append_bytes(buffer, capacity, size, "\n");
}

inline bool append_text_line(char* buffer, std::size_t capacity, std::size_t& size,
                             std::string_view key, const CaptureText& value) noexcept {
    return append_line(buffer, capacity, size, key, value.view());
}

// Serializes the record. Returns the bytes written; a truncated record still ends on a line
// boundary, and `truncated=1` is appended when a line did not fit so the parser never silently
// reports a short frame list as complete.
inline std::size_t compose_crash_record(const CaptureMoment& moment,
                                        const CaptureMetadata& metadata,
                                        const CaptureModuleTable& modules, char* buffer,
                                        std::size_t capacity) noexcept {
    std::size_t size = 0;
    bool complete = append_bytes(buffer, capacity, size, kCrashRecordSchema) &&
                    append_bytes(buffer, capacity, size, "\n");
    complete = complete && append_text_line(buffer, capacity, size, "reason", moment.reason) &&
               append_uint_line(buffer, capacity, size, "code", moment.code) &&
               append_text_line(buffer, capacity, size, "thread", moment.thread) &&
               append_hex_line(buffer, capacity, size, "address", moment.address) &&
               append_text_line(buffer, capacity, size, "app", metadata.app_version) &&
               append_text_line(buffer, capacity, size, "commit", metadata.commit) &&
               append_text_line(buffer, capacity, size, "build", metadata.build_id) &&
               append_text_line(buffer, capacity, size, "platform", metadata.platform) &&
               append_text_line(buffer, capacity, size, "arch", metadata.arch) &&
               append_text_line(buffer, capacity, size, "os", metadata.os) &&
               append_text_line(buffer, capacity, size, "gpu", metadata.gpu) &&
               append_text_line(buffer, capacity, size, "config", metadata.config_hash);

    for (std::size_t index = 0; complete && index < modules.count; ++index) {
        complete = append_bytes(buffer, capacity, size, "module=") &&
                   append_bytes(buffer, capacity, size, modules.names[index]) &&
                   append_bytes(buffer, capacity, size, "|") &&
                   append_hex(buffer, capacity, size, modules.bases[index]) &&
                   append_bytes(buffer, capacity, size, "|") &&
                   append_hex(buffer, capacity, size, modules.sizes[index]) &&
                   append_bytes(buffer, capacity, size, "\n");
    }

    std::size_t unmatched = 0;
    for (std::size_t index = 0; complete && moment.frames != nullptr && index < moment.frame_count;
         ++index) {
        const int owner = modules.index_of(moment.frames[index]);
        if (owner < 0) {
            ++unmatched;
            continue;
        }
        const std::uint64_t offset = moment.frames[index] - modules.bases[owner];
        complete = append_bytes(buffer, capacity, size, "frame=") &&
                   append_decimal(buffer, capacity, size, static_cast<std::uint64_t>(owner)) &&
                   append_bytes(buffer, capacity, size, "|") &&
                   append_hex(buffer, capacity, size, offset) &&
                   append_bytes(buffer, capacity, size, "\n");
    }
    if (!complete) {
        size = 0;
        (void)append_bytes(buffer, capacity, size, kCrashRecordSchema);
        (void)append_bytes(buffer, capacity, size, "\ntruncated=1\n");
        return size;
    }
    // Unmatched program counters (a JIT frame, an unmapped module) are counted, not dropped
    // silently: the parser reports the count and `CrashReporter` marks the report incomplete.
    (void)append_uint_line(buffer, capacity, size, "unmatched", unmatched);
    return size;
}

} // namespace tessera::diagnostics::detail
