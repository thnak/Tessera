// SPDX-License-Identifier: MIT

// Literal lowering and the int/float typing rule (docs/specs/ir-v1.md section 4.1).

#include "expression_lowering_internal.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kLiteral = "TESSERA_IR_LITERAL";

// ir-v1.md section 4.1 fixes `int` at signed 32-bit. The spec is silent on how a numeric literal
// spelling becomes int or float, so slice 5 uses the narrowest lossless mapping and leaves the
// final decision to the type pass (slice 4):
//   - a spelling with `.` or an exponent is a float (1.0, 0.5, 1e3);
//   - an integer spelling that fits int32 is an int (0, 42, 2147483647);
//   - an integer spelling outside int32 stays a float, because the IR has no wider int. This is
//     provisional: slice 4 can reject or retype it once literal types are checked.
bool has_fraction_or_exponent(std::string_view spelling) {
    return spelling.find_first_of(".eE") != std::string_view::npos;
}

std::optional<std::int64_t> int32_value(double number) {
    constexpr double kMin = -2147483648.0;
    constexpr double kMax = 2147483647.0;
    if (!std::isfinite(number) || std::trunc(number) != number || number < kMin || number > kMax) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
}

} // namespace

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
