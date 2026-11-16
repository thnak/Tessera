// SPDX-License-Identifier: MIT

// Parser for the record the crash handler writes. Runs in a healthy process (the next launch), so
// unlike the writer it may allocate freely. The format is the one `compose_crash_record` produces:
// a schema line, `key=value` metadata lines, a `module=name|base|size` table and
// `frame=<module>|<offset>` lines, in that order.

#include "tessera/diagnostics/crash_report.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>

namespace tessera::diagnostics {

namespace {

constexpr std::string_view kSchemaPrefix = "tessera.crash.";
constexpr std::string_view kSchemaV1 = "tessera.crash.v1";

// A `key=value` split, or a failed parse when there is no separator.
bool split_field(std::string_view line, std::string_view& key, std::string_view& value) {
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return false;
    }
    key = line.substr(0, equals);
    value = line.substr(equals + 1);
    return true;
}

bool parse_hex(std::string_view text, std::uint64_t& out) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return false;
    }
    std::uint64_t value = 0;
    const char* begin = text.data() + 2;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

bool parse_decimal(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

void assign(std::string& target, std::string_view value) {
    target.assign(value);
}

} // namespace

std::string_view to_string(CrashParseError error) noexcept {
    switch (error) {
    case CrashParseError::NotACrashRecord:
        return "not a crash record";
    case CrashParseError::Unsupported:
        return "unsupported crash record schema";
    case CrashParseError::Malformed:
        return "malformed crash record";
    }
    return "not a crash record";
}

std::expected<CrashRecord, CrashParseError> parse_crash_record(std::string_view text) {
    if (text.empty()) {
        return std::unexpected(CrashParseError::NotACrashRecord);
    }
    std::size_t cursor = 0;
    // Schema line first: a file that is not ours is rejected before anything else is believed.
    const std::size_t first_end = text.find('\n');
    const std::string_view first =
        first_end == std::string_view::npos ? text : text.substr(0, first_end);
    const std::string_view schema = first.substr(0, first.find_last_not_of(" \t\r") + 1);
    if (schema.substr(0, std::min(schema.size(), kSchemaPrefix.size())) != kSchemaPrefix) {
        return std::unexpected(CrashParseError::NotACrashRecord);
    }
    if (schema != kSchemaV1) {
        return std::unexpected(CrashParseError::Unsupported);
    }
    cursor = first_end == std::string_view::npos ? text.size() : first_end + 1;

    CrashRecord record;
    record.valid = true;
    while (cursor < text.size()) {
        const std::size_t line_end = text.find('\n', cursor);
        const std::string_view line = std::string_view(text).substr(
            cursor, (line_end == std::string_view::npos ? text.size() : line_end) - cursor);
        cursor = line_end == std::string_view::npos ? text.size() : line_end + 1;
        if (line.empty() || line == "\r") {
            continue;
        }
        // `#` lines are the redaction pass's markers (`# env-dropped`): a comment, not data, so a
        // redacted record still parses. The writer never emits one.
        if (line.front() == '#') {
            continue;
        }
        if (line == "truncated=1") {
            record.truncated = true;
            continue;
        }
        std::string_view key;
        std::string_view value;
        if (!split_field(line, key, value)) {
            return std::unexpected(CrashParseError::Malformed);
        }
        if (key == "reason") {
            assign(record.reason, value);
        } else if (key == "code") {
            std::uint64_t code = 0;
            if (!parse_decimal(value, code)) {
                return std::unexpected(CrashParseError::Malformed);
            }
            record.code = code;
        } else if (key == "thread") {
            assign(record.thread, value);
        } else if (key == "address") {
            std::uint64_t address = 0;
            if (!parse_hex(value, address)) {
                return std::unexpected(CrashParseError::Malformed);
            }
            record.address = address;
        } else if (key == "app") {
            assign(record.app_version, value);
        } else if (key == "commit") {
            assign(record.commit, value);
        } else if (key == "build") {
            assign(record.build_id, value);
        } else if (key == "platform") {
            assign(record.platform, value);
        } else if (key == "arch") {
            assign(record.arch, value);
        } else if (key == "os") {
            assign(record.os, value);
        } else if (key == "gpu") {
            assign(record.gpu, value);
        } else if (key == "config") {
            assign(record.config_hash, value);
        } else if (key == "unmatched") {
            std::uint64_t count = 0;
            if (!parse_decimal(value, count)) {
                return std::unexpected(CrashParseError::Malformed);
            }
            record.unmatched_frames = count;
        } else if (key == "module") {
            const std::size_t first_bar = value.find('|');
            const std::size_t second_bar = first_bar == std::string_view::npos
                                               ? std::string_view::npos
                                               : value.find('|', first_bar + 1);
            if (first_bar == std::string_view::npos || second_bar == std::string_view::npos) {
                return std::unexpected(CrashParseError::Malformed);
            }
            CrashModule module;
            // `redact_path` again on read: a hand-edited or older record must not be able to
            // smuggle an absolute path into a report through the module table.
            module.name = redact_path(value.substr(0, first_bar));
            if (!parse_hex(value.substr(first_bar + 1, second_bar - first_bar - 1), module.base) ||
                !parse_hex(value.substr(second_bar + 1), module.size)) {
                return std::unexpected(CrashParseError::Malformed);
            }
            record.modules.push_back(std::move(module));
        } else if (key == "frame") {
            const std::size_t bar = value.find('|');
            if (bar == std::string_view::npos) {
                return std::unexpected(CrashParseError::Malformed);
            }
            std::uint64_t index = 0;
            CrashRawFrame frame;
            if (!parse_decimal(value.substr(0, bar), index) ||
                !parse_hex(value.substr(bar + 1), frame.offset)) {
                return std::unexpected(CrashParseError::Malformed);
            }
            if (index >= record.modules.size()) {
                // A frame naming a module that is not in the table would index out of range later;
                // count it as unmatched instead of dropping it silently.
                ++record.unmatched_frames;
                continue;
            }
            frame.module = static_cast<std::size_t>(index);
            record.frames.push_back(frame);
        } else {
            return std::unexpected(CrashParseError::Malformed);
        }
    }
    return record;
}

std::vector<CrashFrame> crash_record_frames(const CrashRecord& record) {
    std::vector<CrashFrame> frames;
    frames.reserve(record.frames.size());
    for (const CrashRawFrame& raw : record.frames) {
        CrashFrame frame;
        if (raw.module < record.modules.size()) {
            frame.module = record.modules[raw.module].name;
        }
        frame.offset = raw.offset;
        frames.push_back(frame);
    }
    return frames;
}

CrashMetadata crash_record_metadata(const CrashRecord& record) {
    CrashMetadata metadata;
    metadata.app_version = record.app_version;
    metadata.commit = record.commit;
    metadata.build_id = record.build_id;
    metadata.platform = record.platform;
    metadata.arch = record.arch;
    metadata.os = record.os;
    metadata.gpu = record.gpu;
    metadata.config_hash = record.config_hash;
    metadata.crash_reason = record.reason;
    metadata.thread = record.thread;
    return metadata;
}

} // namespace tessera::diagnostics
