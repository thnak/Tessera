// SPDX-License-Identifier: MIT

// Template literal lowering: cooked text parts interleaved with lowered substitutions.

#include "expression_lowering_internal.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kMalformed = "TESSERA_IR_MALFORMED";

} // namespace

ir::ExprPtr lower_template(LoweringState& state, const jsx::Expr& expr) {
    // The parser produces strings.size() == operands.size() + 1: text, substitution, text, ...,
    // text (docs/specs/jsx-subset-v1.md section 3.4).
    if (expr.template_strings.size() != expr.operands.size() + 1) {
        fail(state, kMalformed, "a template needs one more text part than substitutions",
             expr.span);
        return nullptr;
    }
    std::vector<ir::TemplatePart> parts;
    parts.reserve(expr.template_strings.size() + expr.operands.size());
    for (std::size_t i = 0; i < expr.template_strings.size(); ++i) {
        // Keep every text run, including an empty one, so the parts stay a faithful alternation
        // of the source (the IR writer serialises an empty part as "").
        parts.push_back(ir::text_part(std::string(expr.template_strings[i])));
        if (i == expr.operands.size()) {
            break;
        }
        if (!expr.operands[i]) {
            fail(state, kMalformed, "a template substitution is missing", expr.span);
            return nullptr;
        }
        ir::ExprPtr value = lower_expr(state, *expr.operands[i]);
        if (!value) {
            return nullptr;
        }
        parts.push_back(ir::expr_part(std::move(value)));
    }
    return ir::make_template(std::move(parts));
}

} // namespace tessera::compiler::lowering::internal
