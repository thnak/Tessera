// SPDX-License-Identifier: MIT

// Call lowering: only the pure functions of docs/specs/ir-v1.md section 4.2, named without a
// namespace.

#include "expression_lowering_internal.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kUnsupported = "TESSERA_IR_UNSUPPORTED";
constexpr std::string_view kMalformed = "TESSERA_IR_MALFORMED";

constexpr std::string_view kFunctions[] = {"min",     "max",    "clamp",       "abs",
                                           "floor",   "ceil",   "round",       "toInt",
                                           "toFloat", "length", "formatNumber"};

bool is_pure_function(std::string_view name) {
    for (const std::string_view function : kFunctions) {
        if (function == name) {
            return true;
        }
    }
    return false;
}

} // namespace

ir::ExprPtr lower_call(LoweringState& state, const jsx::Expr& expr) {
    if (expr.operands.empty() || !expr.operands[0]) {
        fail(state, kMalformed, "a call needs a callee", expr.span);
        return nullptr;
    }
    const jsx::Expr& callee = *expr.operands[0];
    if (callee.kind != jsx::ExprKind::Identifier) {
        fail(state, kUnsupported,
             "only a bare pure function name can be called; a method call has no IR form "
             "(ir-v1.md section 4.2)",
             callee.span);
        return nullptr;
    }
    if (!is_pure_function(callee.text)) {
        fail(state, kUnsupported,
             "`" + callee.text + "` is not a pure function of ir-v1.md section 4.2", callee.span,
             "call only min, max, clamp, abs, floor, ceil, round, toInt, toFloat, length or "
             "formatNumber");
        return nullptr;
    }
    std::vector<ir::ExprPtr> args;
    args.reserve(expr.operands.size() - 1);
    for (std::size_t i = 1; i < expr.operands.size(); ++i) {
        if (!expr.operands[i]) {
            fail(state, kMalformed, "a call argument is missing", expr.span);
            return nullptr;
        }
        ir::ExprPtr arg = lower_expr(state, *expr.operands[i]);
        if (!arg) {
            return nullptr;
        }
        args.push_back(std::move(arg));
    }
    return ir::make_call(std::string(callee.text), std::move(args));
}

} // namespace tessera::compiler::lowering::internal
