// SPDX-License-Identifier: MIT

// Shared helpers of hook lowering: first-error reporting, callee names and expressions.

#include "hook_lowering_internal.hpp"
#include "tessera/compiler/lowering/expression_lowering.hpp"

#include <utility>

namespace tessera::compiler::lowering::internal {

void fail_hook(HookState& state, std::string_view code, std::string_view message, jsx::Span span,
               std::string_view suggestion) {
    if (state.error) {
        return; // keep the first error; a later hook does not replace it
    }
    state.error = jsx::Diagnostic{code,
                                  jsx::Severity::Error,
                                  std::string(message),
                                  state.context.source_map.resolve(span),
                                  std::string(suggestion),
                                  std::nullopt};
}

std::string callee_name(const jsx::Expr& call) {
    if (call.kind != jsx::ExprKind::Call || call.operands.empty() || !call.operands[0]) {
        return {};
    }
    const jsx::Expr& callee = *call.operands[0];
    return callee.kind == jsx::ExprKind::Identifier ? callee.text : std::string();
}

ir::ExprPtr lower_component_expression(HookState& state, const jsx::Expr& expr,
                                       const NameResolver& names) {
    const ExpressionLoweringContext context{state.context.source_map, state.context.file, &names};
    ExpressionLoweringResult lowered = lower_expression(expr, context);
    if (!lowered) {
        if (!state.error && !lowered.error().empty()) {
            state.error = lowered.error().front();
        }
        if (!state.error) {
            fail_hook(state, "TESSERA_IR_MALFORMED", "the expression produced no IR", expr.span);
        }
        return nullptr;
    }
    return std::move(*lowered);
}

} // namespace tessera::compiler::lowering::internal
