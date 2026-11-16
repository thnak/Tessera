// SPDX-License-Identifier: MIT

// The `tessera.symbols.v1` text format, shared by the producers and the resolver. Text on purpose:
// a symbol artifact has to survive CI upload/download and be inspectable with `cat`, the repository
// already ships text artifacts (golden JSON, traces), and it needs no new dependency.

#include "tessera/diagnostics/crash_symbols.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>

namespace tessera::diagnostics {

namespace {

constexpr std::string_view kSchema = "tessera.symbols.v1";

bool parse_hex(std::string_view text, std::uint64_t& out) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return false;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data() + 2, text.data() + text.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
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

void append_hex(std::string& out, std::uint64_t value) {
    constexpr char kDigits[] = "0123456789abcdef";
    char digits[16] = {};
    std::size_t count = 0;
    do {
        digits[count] = kDigits[value & 0xFU];
        ++count;
        value >>= 4U;
    } while (value != 0U);
    out += "0x";
    for (std::size_t index = 0; index < count; ++index) {
        out.push_back(digits[count - 1 - index]);
    }
}

} // namespace

std::expected<SymbolFile, std::string> parse_symbol_file(std::string_view text) {
    SymbolFile file;
    std::size_t cursor = 0;
    bool saw_schema = false;
    while (cursor <= text.size()) {
        const std::size_t line_end = text.find('\n', cursor);
        const std::string_view line =
            text.substr(cursor, line_end == std::string_view::npos ? std::string_view::npos
                                                                   : line_end - cursor);
        cursor = line_end == std::string_view::npos ? text.size() + 1 : line_end + 1;
        if (line.empty()) {
            continue;
        }
        if (!saw_schema) {
            if (line != kSchema) {
                return std::unexpected("not a tessera.symbols.v1 file");
            }
            saw_schema = true;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected("malformed symbol line");
        }
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);
        if (key == "build") {
            file.build_id.assign(value);
        } else if (key == "module") {
            const std::size_t first = value.find('|');
            const std::size_t second = first == std::string_view::npos ? std::string_view::npos
                                                                       : value.find('|', first + 1);
            if (first == std::string_view::npos || second == std::string_view::npos) {
                return std::unexpected("malformed module line");
            }
            CrashModule module;
            module.name = redact_path(value.substr(0, first));
            if (!parse_hex(value.substr(first + 1, second - first - 1), module.base) ||
                !parse_hex(value.substr(second + 1), module.size)) {
                return std::unexpected("malformed module address");
            }
            file.modules.push_back(std::move(module));
        } else if (key == "func") {
            const std::size_t first = value.find('|');
            if (first == std::string_view::npos) {
                return std::unexpected("malformed func line");
            }
            const std::size_t second = value.find('|', first + 1);
            const std::size_t third = second == std::string_view::npos
                                          ? std::string_view::npos
                                          : value.find('|', second + 1);
            if (second == std::string_view::npos || third == std::string_view::npos) {
                return std::unexpected("malformed func line");
            }
            std::uint64_t module = 0;
            SymbolEntry entry;
            if (!parse_decimal(value.substr(0, first), module) ||
                !parse_hex(value.substr(first + 1, second - first - 1), entry.start) ||
                !parse_hex(value.substr(second + 1, third - second - 1), entry.size)) {
                return std::unexpected("malformed func fields");
            }
            entry.module = static_cast<std::size_t>(module);
            entry.name.assign(value.substr(third + 1));
            if (entry.name.empty()) {
                return std::unexpected("func with no name");
            }
            file.entries.push_back(std::move(entry));
        } else {
            return std::unexpected("unknown symbol key");
        }
    }
    if (!saw_schema) {
        return std::unexpected("empty symbol file");
    }
    file.valid = true;
    return file;
}

std::string write_symbol_file(const SymbolFile& file) {
    std::string out;
    out.reserve(1024);
    out += kSchema;
    out.push_back('\n');
    if (!file.build_id.empty()) {
        out += "build=";
        out += file.build_id;
        out.push_back('\n');
    }
    for (const CrashModule& module : file.modules) {
        out += "module=";
        out += redact_path(module.name);
        out.push_back('|');
        append_hex(out, module.base);
        out.push_back('|');
        append_hex(out, module.size);
        out.push_back('\n');
    }
    for (const SymbolEntry& entry : file.entries) {
        out += "func=";
        out += std::to_string(entry.module);
        out.push_back('|');
        append_hex(out, entry.start);
        out.push_back('|');
        append_hex(out, entry.size);
        out.push_back('|');
        out += entry.name;
        out.push_back('\n');
    }
    return out;
}

SymbolizeResult symbolize_record(const CrashRecord& record, const SymbolFile& file) {
    SymbolizeResult result;
    result.build_id_ok =
        !file.build_id.empty() && !record.build_id.empty() && file.build_id == record.build_id;
    result.frames.reserve(record.frames.size());
    for (const CrashRawFrame& raw : record.frames) {
        CrashFrame frame;
        if (raw.module < record.modules.size()) {
            frame.module = record.modules[raw.module].name;
        }
        frame.offset = raw.offset;
        if (result.build_id_ok) {
            // Entries are sorted by (module, start); the covering entry is the last start that is
            // <= the offset. A size of 0 means "until the next entry", which a linker map needs.
            const SymbolEntry* best = nullptr;
            for (const SymbolEntry& entry : file.entries) {
                if (entry.module != raw.module || entry.start > raw.offset) {
                    continue;
                }
                if (best == nullptr || entry.start >= best->start) {
                    best = &entry;
                }
            }
            if (best != nullptr && (best->size == 0 || raw.offset < best->start + best->size)) {
                frame.symbol = best->name;
            }
        }
        if (frame.symbol.empty()) {
            ++result.unsymbolicated;
        }
        result.frames.push_back(frame);
    }
    return result;
}

CrashMetadata symbolized_metadata(const CrashRecord& record, const SymbolizeResult& result) {
    CrashMetadata metadata = crash_record_metadata(record);
    metadata.frames = result.frames;
    return metadata;
}

} // namespace tessera::diagnostics
