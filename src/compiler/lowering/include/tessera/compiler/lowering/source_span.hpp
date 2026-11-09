// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/source_span.hpp"
#include "tessera/compiler/jsx/source_map.hpp"

#include <string_view>

namespace tessera::compiler::lowering {

// Converts one AST span to the IR span of docs/specs/ir-v1.md section 8:
// - offset and length stay byte counts into the UTF-8 source, so a BOM counts in the offset
//   (docs/design/jsx-ir-lowering.md decision 16);
// - line and column are 1-based and columns count Unicode code points (SourceMap derives them);
// - endLine/endColumn are the exclusive end of the span;
// - `file` is the caller's project-relative path with POSIX separators.
//
// IR expressions have no source field in v1. The conversion exists for diagnostics (which carry
// it through jsx::Diagnostic) and for the nodes, bindings and effects of slices 6 to 8.
[[nodiscard]] ir::SourceSpan to_source_span(const jsx::SourceMap& map, jsx::Span span,
                                            std::string_view file);

} // namespace tessera::compiler::lowering
