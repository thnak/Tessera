// SPDX-License-Identifier: MIT

// Literal lowering and the int/float typing rule (docs/specs/jsx-subset-v1.md section 5,
// "Literal types"; docs/specs/ir-v1.md section 4.1).

#include "expression_lowering_internal.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kLiteral = "TESSERA_IR_LITERAL";

// jsx-subset-v1.md section 5: a literal without a fraction or exponent is `int`; one with them is
// `float`. The expected-type override (an integer spelling in a `float` position becomes float)
// needs the type table of slice 4, so lowering only decides the base type here.
std::string float_spelling_suggestion(std::string_view spelling) {
    if (spelling.empty()) {
        return "write the literal with a fraction or exponent if a float was intended";
    }
    return "write `" + std::string(spelling) +
           ".0` if a float was intended (the expected-type override needs the type pass)";
}

} // namespace

bool has_fraction_or_exponent(std::string_view spelling) {
    return spelling.find_first_of(".eE") != std::string_view::npos;
}

// ir-v1.md section 4.1 fixes `int` at signed 32-bit. A value outside that range has no IR int
// form, so lowering rejects it instead of silently changing its type to float.
std::optional<std::int64_t> int32_value(double number) {
    constexpr double kMin = -2147483648.0;
    constexpr double kMax = 2147483647.0;
    if (!std::isfinite(number) || std::trunc(number) != number || number < kMin || number > kMax) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
}

ir::ExprPtr lower_literal(LoweringState& state, const jsx::Expr& expr) {
    switch (expr.kind) {
    case jsx::ExprKind::Number:
        if (!std::isfinite(expr.number)) {
            fail(state, kLiteral, "a number literal must be finite", expr.span);
            return nullptr;
        }
        if (!has_fraction_or_exponent(expr.text)) {
            if (const std::optional<std::int64_t> integer = int32_value(expr.number)) {
                return ir::make_literal(*integer);
            }
            fail(state, kLiteral,
                 "integer literal `" + expr.text +
                     "` is outside the IR int range (signed 32-bit, ir-v1.md section 4.1)",
                 expr.span, float_spelling_suggestion(expr.text));
            return nullptr;
        }
        return ir::make_literal(expr.number);
    case jsx::ExprKind::String:
        return ir::make_literal(expr.text);
    case jsx::ExprKind::Boolean:
        return ir::make_literal(expr.boolean);
    case jsx::ExprKind::Null:
        return ir::make_literal(ir::Null{});
    default:
        fail(state, kLiteral, "the literal expression has an unexpected kind", expr.span);
        return nullptr;
    }
}

} // namespace tessera::compiler::lowering::internal
