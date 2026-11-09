// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/lowering/expression_lowering.hpp"

#include <optional>
#include <string>
#include <string_view>

// Internal seam between the expression lowerer and its parts (literal, reference, call,
// template). Not installed: only src/compiler/lowering includes it.
namespace tessera::compiler::lowering::internal {

struct LoweringState {
    const ExpressionLoweringContext& context;
    std::optional<jsx::Diagnostic> error; // the first error; later calls are ignored
};

[[nodiscard]] ir::ExprPtr lower_expr(LoweringState& state, const jsx::Expr& expr);
[[nodiscard]] ir::ExprPtr lower_literal(LoweringState& state, const jsx::Expr& expr);
[[nodiscard]] ir::ExprPtr lower_reference(LoweringState& state, const jsx::Expr& expr);
[[nodiscard]] ir::ExprPtr lower_template(LoweringState& state, const jsx::Expr& expr);
[[nodiscard]] ir::ExprPtr lower_call(LoweringState& state, const jsx::Expr& expr);

// Reports a TESSERA_IR_* error at `span` unless one was already reported. The diagnostic keeps a
// string_view code, so `code` must point at a string literal.
void fail(LoweringState& state, std::string_view code, std::string message, jsx::Span span,
          std::string suggestion = {});

} // namespace tessera::compiler::lowering::internal
