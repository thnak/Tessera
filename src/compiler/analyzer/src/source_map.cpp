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
}

Location SourceMap::locate(std::uint32_t offset) const {
    offset = std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(source_.size()));
    const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    const auto line_index = static_cast<std::size_t>(it - line_starts_.begin()) - 1;
    std::uint32_t begin = line_starts_[line_index];
    if (line_index == 0) {
        begin = std::min(bom_length_, offset);
    }
    std::uint32_t column = 1;
    for (std::uint32_t i = begin; i < offset; ++i) {
        // Continuation bytes (10xxxxxx) belong to the previous code point.
        if ((static_cast<unsigned char>(source_[i]) & 0xC0U) != 0x80U) {
            ++column;
        }
    }
    return Location{static_cast<std::uint32_t>(line_index + 1), column};
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
