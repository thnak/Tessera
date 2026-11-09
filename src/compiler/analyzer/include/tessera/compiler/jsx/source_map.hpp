// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::compiler::jsx {

// Compact source range: byte offset and byte length into the UTF-8 source. AST nodes and tokens
// store only this; line and column are derived on demand by SourceMap.
struct Span {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] std::uint32_t end() const {
        return offset + length;
    }
};

// 1-based line and column. A column counts Unicode code points from the start of the line.
struct Location {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

// A span with the fields of docs/specs/jsx-subset-v1.md section 13.2.
struct ResolvedSpan {
    Span span;
    Location start;
    Location end;
};

// Maps byte offsets to line/column. LF ends a line, so CRLF is one line break; a leading UTF-8
// byte order mark is not counted as a column.
class SourceMap {
public:
    explicit SourceMap(std::string_view source);

    [[nodiscard]] Location locate(std::uint32_t offset) const;
    [[nodiscard]] ResolvedSpan resolve(Span span) const;
    [[nodiscard]] std::string_view source() const {
        return source_;
    }
    [[nodiscard]] std::string_view slice(Span span) const;

private:
    std::string_view source_;
    std::uint32_t bom_length_ = 0;
    std::vector<std::uint32_t> line_starts_;
};

} // namespace tessera::compiler::jsx
