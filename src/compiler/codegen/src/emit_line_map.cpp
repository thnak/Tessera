// SPDX-License-Identifier: MIT

#include "emit_line_map.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

constexpr std::string_view kMarkerPrefix = "//@tessera-line";

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

std::string_view trim_left(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    return text;
}

// The index a begin marker names. A marker whose tail is not a plain integer is ordinary text, so a
// corrupted input fails loudly downstream instead of silently mapping to line 0.
bool parse_marker_index(std::string_view text, std::size_t& index) {
    text = trim_left(text);
    if (!starts_with(text, kMarkerPrefix)) {
        return false;
    }
    text.remove_prefix(kMarkerPrefix.size());
    if (text.empty() || text.front() != ' ') {
        return false;
    }
    text.remove_prefix(1);
    if (text.empty()) {
        return false;
    }
    std::size_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    index = value;
    return true;
}

bool is_end_marker(std::string_view text) {
    return trim_left(text) == kLineMapEnd;
}

std::string escaped_file(std::string_view file) {
    std::string out;
    out.reserve(file.size() + 2);
    for (const char c : file) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

} // namespace

bool line_map_usable(const ir::SourceSpan& span) {
    if (span.file.empty() || span.line == 0) {
        return false;
    }
    return span.file.find('"') == std::string_view::npos &&
           span.file.find('\n') == std::string_view::npos;
}

std::string marker_line(std::size_t index) {
    return std::string(kMarkerPrefix) + " " + std::to_string(index);
}

std::string line_directive(std::uint32_t line, std::string_view file) {
    return "#line " + std::to_string(line) + " \"" + escaped_file(file) + "\"";
}

std::string apply_line_map(std::string_view generated_name, std::string_view text,
                           const LineMapMarkers& markers) {
    const std::vector<std::string_view> lines = split_lines(text);
    std::vector<std::string> out;
    out.reserve(lines.size() + 8);
    bool in_region = false;
    bool region_mapped = false;

    // `out.size() + 1` is the 1-based number of the line the next push will occupy, and the pushed
    // directive takes it, so the restoring directive names the line after itself: the first line of
    // generated text again. That is what keeps a later diagnostic at its own `.g.hpp` line instead
    // of drifting by the directives emitted above it.
    const auto close_region = [&] {
        if (region_mapped) {
            out.push_back(
                line_directive(static_cast<std::uint32_t>(out.size() + 2), generated_name));
        }
        in_region = false;
        region_mapped = false;
    };

    for (const std::string_view line : lines) {
        std::size_t index = 0;
        if (parse_marker_index(line, index)) {
            if (in_region) {
                close_region();
            }
            in_region = true;
            const bool usable =
                index < markers.spans.size() && line_map_usable(markers.spans[index]);
            region_mapped = usable;
            if (usable) {
                out.push_back(line_directive(markers.spans[index].line, markers.spans[index].file));
            }
            continue;
        }
        if (is_end_marker(line)) {
            if (in_region) {
                close_region();
            }
            continue;
        }
        out.push_back(std::string(line));
    }
    if (in_region) {
        close_region();
    }

    std::string result;
    result.reserve(text.size() + markers.spans.size() * 32);
    for (const std::string& line : out) {
        result += line;
        result += '\n';
    }
    return result;
}

} // namespace tessera::compiler::codegen::internal
