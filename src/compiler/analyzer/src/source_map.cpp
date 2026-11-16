// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/source_map.hpp"

#include <algorithm>

namespace tessera::compiler::jsx {

SourceMap::SourceMap(std::string_view source)
    : source_(source) {
    if (source_.size() >= 3 && static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB &&
        static_cast<unsigned char>(source_[2]) == 0xBF) {
        bom_length_ = 3;
    }
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < source_.size(); ++i) {
        if (source_[i] == '\n') {
            line_starts_.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }

    // Index the code-point boundaries of any line that is not pure ASCII. Locating a column by
    // rescanning the line made lowering O(line length) per node, which turned a legal 1 MiB
    // single-line file into a 28-second build (security review finding S3). A pure ASCII line needs
    // no index: its column is the byte distance plus one.
    line_index_.resize(line_starts_.size());
    for (std::size_t line = 0; line < line_starts_.size(); ++line) {
        const std::uint32_t begin = line_starts_[line];
        const std::uint32_t end = line + 1 < line_starts_.size()
                                      ? line_starts_[line + 1]
                                      : static_cast<std::uint32_t>(source_.size());
        LineIndex& index = line_index_[line];
        index.first_offset = begin;
        for (std::uint32_t i = begin; i < end; ++i) {
            const auto byte = static_cast<unsigned char>(source_[i]);
            // A non-continuation byte starts a code point; record it so column = index + 1.
            if ((byte & 0xC0U) != 0x80U) {
                index.code_points.push_back(i);
            }
        }
        // A pure ASCII line is exactly its code points one per byte, so the index would be pure
        // overhead; drop it and let column_for use arithmetic.
        if (index.code_points.size() == static_cast<std::size_t>(end - begin)) {
            index.code_points.clear();
            index.code_points.shrink_to_fit();
        }
    }
}

std::uint32_t SourceMap::column_for(std::size_t line_index, std::uint32_t offset) const {
    const LineIndex& index = line_index_[line_index];
    // A leading byte order mark is not a column, so line 0 starts after it.
    const std::uint32_t begin =
        line_index == 0 ? std::min(bom_length_, offset) : index.first_offset;
    if (offset <= begin) {
        return 1;
    }
    if (index.code_points.empty()) {
        // A pure ASCII line has one code point per byte, so the column is the distance plus one.
        return offset - begin + 1;
    }
    // Exactly the code-point starts in [begin, offset), plus one for the first column.
    const auto first = std::lower_bound(index.code_points.begin(), index.code_points.end(), begin);
    const auto last =
        std::upper_bound(index.code_points.begin(), index.code_points.end(), offset - 1);
    return static_cast<std::uint32_t>(last - first) + 1;
}

Location SourceMap::locate(std::uint32_t offset) const {
    offset = std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(source_.size()));
    const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    const auto line_index = static_cast<std::size_t>(it - line_starts_.begin()) - 1;
    return Location{static_cast<std::uint32_t>(line_index + 1), column_for(line_index, offset)};
}

ResolvedSpan SourceMap::resolve(Span span) const {
    return ResolvedSpan{span, locate(span.offset), locate(span.end())};
}

std::string_view SourceMap::slice(Span span) const {
    if (span.offset > source_.size()) {
        return {};
    }
    return source_.substr(span.offset, span.length);
}

} // namespace tessera::compiler::jsx
