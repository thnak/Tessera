// SPDX-License-Identifier: MIT

#include <tessera/compiler/merger.hpp>

#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace tessera::compiler {
namespace {

constexpr std::string_view kStartMarker = "// [DEVELOPER_LOGIC_START:";
constexpr std::string_view kEndMarker = "// [DEVELOPER_LOGIC_END:";

std::string_view trim(std::string_view s) {
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

// Beginning of the line containing `pos` (including the preceding newline).
std::size_t line_begin(std::string_view text, std::size_t pos) {
    if (pos == 0) {
        return 0;
    }
    const std::size_t newline = text.rfind('\n', pos - 1);
    return newline == std::string_view::npos ? 0 : newline + 1;
}

// One past the end of the line containing `pos` (including its newline, if any).
std::size_t line_end(std::string_view text, std::size_t pos) {
    const std::size_t newline = text.find('\n', pos);
    return newline == std::string_view::npos ? text.size() : newline + 1;
}

std::string parse_id(std::string_view line, std::string_view marker) {
    const std::size_t marker_pos = line.find(marker);
    if (marker_pos == std::string_view::npos) {
        return {};
    }
    const std::size_t begin = marker_pos + marker.size();
    const std::size_t close = line.find(']', begin);
    if (close == std::string_view::npos) {
        return {};
    }
    return std::string(trim(line.substr(begin, close - begin)));
}

struct ParsedRegion {
    std::string id;
    std::size_t start_marker_begin = 0;
    std::size_t body_begin = 0;
    std::size_t body_end = 0;
    std::size_t end_marker_begin = 0;
    std::size_t end_marker_end = 0;
};

std::vector<ParsedRegion> scan(std::string_view text) {
    std::vector<ParsedRegion> regions;
    std::size_t cursor = 0;

    while (true) {
        const std::size_t start = text.find(kStartMarker, cursor);
        if (start == std::string_view::npos) {
            break;
        }

        ParsedRegion region{};
        region.start_marker_begin = line_begin(text, start);
        const std::size_t start_marker_end = line_end(text, start);
        region.id = parse_id(
            text.substr(region.start_marker_begin, start_marker_end - region.start_marker_begin),
            kStartMarker);
        if (region.id.empty()) {
            throw std::runtime_error("malformed protected-region start marker");
        }

        const std::size_t end = text.find(kEndMarker, start_marker_end);
        if (end == std::string_view::npos) {
            throw std::runtime_error("unterminated protected region: " + region.id);
        }

        region.end_marker_begin = line_begin(text, end);
        region.end_marker_end = line_end(text, end);
        const std::string end_id = parse_id(
            text.substr(region.end_marker_begin, region.end_marker_end - region.end_marker_begin),
            kEndMarker);
        if (end_id != region.id) {
            throw std::runtime_error("protected-region id mismatch: start '" + region.id +
                                     "' end '" + end_id + "'");
        }

        region.body_begin = start_marker_end;
        region.body_end = region.end_marker_begin;
        if (region.body_end < region.body_begin) {
            throw std::runtime_error("protected-region body ordering error: " + region.id);
        }

        regions.push_back(region);
        cursor = region.end_marker_end;
    }

    return regions;
}

} // namespace

std::vector<ProtectedRegion> extract_regions(std::string_view text) {
    std::vector<ProtectedRegion> result;
    for (const ParsedRegion& region : scan(text)) {
        result.push_back({region.id, std::string(text.substr(
                                         region.body_begin, region.body_end - region.body_begin))});
    }
    return result;
}

MergeResult merge(std::string_view generated, std::string_view existing) {
    MergeResult result;

    std::unordered_map<std::string, std::string> existing_bodies;
    for (const ProtectedRegion& region : extract_regions(existing)) {
        existing_bodies[region.id] = region.body;
    }

    std::unordered_set<std::string> seen_ids;
    std::size_t cursor = 0;

    for (const ParsedRegion& region : scan(generated)) {
        // Everything before the body, including the generated start marker line.
        result.text.append(generated.substr(cursor, region.body_begin - cursor));

        const auto it = existing_bodies.find(region.id);
        if (it != existing_bodies.end()) {
            result.text.append(it->second);
        } else {
            result.text.append(
                generated.substr(region.body_begin, region.body_end - region.body_begin));
        }

        // Regenerated end marker line.
        result.text.append(generated.substr(region.end_marker_begin,
                                            region.end_marker_end - region.end_marker_begin));

        seen_ids.insert(region.id);
        cursor = region.end_marker_end;
    }

    result.text.append(generated.substr(cursor));

    for (const auto& [id, body] : existing_bodies) {
        (void)body;
        if (seen_ids.find(id) == seen_ids.end()) {
            result.warnings.push_back("existing protected region '" + id +
                                      "' is not present in the generated file");
        }
    }

    return result;
}

} // namespace tessera::compiler
