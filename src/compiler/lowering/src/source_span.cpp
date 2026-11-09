// SPDX-License-Identifier: MIT

// Span conversion and the shared diagnostic helper (docs/design/jsx-ir-lowering.md section 4).

#include "expression_lowering_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::lowering {

ir::SourceSpan to_source_span(const jsx::SourceMap& map, jsx::Span span, std::string_view file) {
    const jsx::ResolvedSpan resolved = map.resolve(span);
    return ir::SourceSpan{std::string(file),   span.offset,           span.length,
                          resolved.start.line, resolved.start.column, resolved.end.line,
                          resolved.end.column};
}

} // namespace tessera::compiler::lowering

namespace tessera::compiler::lowering::internal {

void fail(LoweringState& state, std::string_view code, std::string message, jsx::Span span,
          std::string suggestion) {
    if (state.error) {
        return; // keep the first error; a later subtree does not replace it
    }
    state.error = jsx::Diagnostic{code,
                                  jsx::Severity::Error,
                                  std::move(message),
                                  state.context.source_map.resolve(span),
                                  std::move(suggestion),
                                  std::nullopt};
}

} // namespace tessera::compiler::lowering::internal
