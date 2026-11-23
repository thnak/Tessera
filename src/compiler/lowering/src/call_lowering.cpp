// SPDX-License-Identifier: MIT

// Call lowering: the pure functions of docs/specs/ir-v1.md section 4.2, named without a namespace,
// and the pure methods of ADR-066 item 3 (`filter`, `slice` on a list; the total string methods).

#include "expression_lowering_internal.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kUnsupported = "TESSERA_IR_UNSUPPORTED";
constexpr std::string_view kMalformed = "TESSERA_IR_MALFORMED";

constexpr std::string_view kFunctions[] = {"min", "max", "clamp", "abs", "floor", "ceil", "round",
                                           "toInt", "toFloat", "length", "formatNumber",
                                           // Issue #803: the strict whole-string parses. Each
                                           // takes a fallback, so they are functions, not
                                           // methods.
                                           "parseIntOr", "parseFloatOr"};

// ADR-066 item 3: the total string methods. Each is total (never throws) and locale-independent, so
// it is a pure function of its receiver and arguments like the numeric ones. `slice` is shared with
// the list form: the receiver's type decides which IR kind it lowers to.
constexpr std::string_view kStringMethods[] = {"trim",       "toUpperCase", "toLowerCase",
                                               "startsWith", "slice",       "toFixed"};

bool is_pure_function(std::string_view name) {
    for (const std::string_view function : kFunctions) {
        if (function == name) {
            return true;
        }
    }
    return false;
}

bool is_string_method(std::string_view name) {
    for (const std::string_view method : kStringMethods) {
        if (method == name) {
            return true;
        }
    }
    return false;
}

// `list.filter(item => <bool>)` (ADR-066 item 3). The predicate is a single-expression arrow with
// one parameter, because the IR carries the item name and one expression, not a body.
ir::ExprPtr lower_filter(LoweringState& state, const jsx::Expr& call, const jsx::Expr& receiver) {
    if (call.operands.size() != 2 || !call.operands[1] ||
        call.operands[1]->kind != jsx::ExprKind::Arrow) {
        fail(state, kUnsupported,
             "`filter` takes exactly one arrow predicate: `list.filter(item => <bool>)`",
             call.span);
        return nullptr;
    }
    const jsx::Expr& arrow = *call.operands[1];
    if (arrow.params.size() != 1 || arrow.arrow_block_body || arrow.operands.size() != 1 ||
        !arrow.operands[0]) {
        fail(state, kUnsupported,
             "the `filter` predicate is a single expression with one parameter, not a block",
             arrow.span, "write `list.filter(item => item.on)`");
        return nullptr;
    }
    ir::ExprPtr list = lower_expr(state, receiver);
    if (!list) {
        return nullptr;
    }
    // The predicate sees the item name, so it is bound for the predicate's own lowering and shadows
    // any outer name of the same spelling.
    const std::string item = arrow.params.front();
    state.scope.push_back(item);
    ir::ExprPtr predicate = lower_expr(state, *arrow.operands[0]);
    state.scope.pop_back();
    if (!predicate) {
        return nullptr;
    }
    return ir::make_list_filter(std::move(list), item, std::move(predicate));
}

// `list.slice(start[, end])` (ADR-066 item 3). The bounds are int expressions; the IR clamps them,
// so an out-of-range bound is not an error.
ir::ExprPtr lower_list_slice(LoweringState& state, const jsx::Expr& call,
                             const jsx::Expr& receiver) {
    if (call.operands.size() < 2 || call.operands.size() > 3) {
        fail(state, kUnsupported, "`slice` takes one or two bounds: `list.slice(start[, end])`",
             call.span);
        return nullptr;
    }
    ir::ExprPtr list = lower_expr(state, receiver);
    if (!list) {
        return nullptr;
    }
    std::vector<ir::ExprPtr> bounds;
    for (std::size_t i = 1; i < call.operands.size(); ++i) {
        if (!call.operands[i]) {
            fail(state, kMalformed, "a `slice` bound is missing", call.span);
            return nullptr;
        }
        ir::ExprPtr bound = lower_expr(state, *call.operands[i]);
        if (!bound) {
            return nullptr;
        }
        bounds.push_back(std::move(bound));
    }
    return ir::make_list_slice(std::move(list), std::move(bounds));
}

// A pure string method (ADR-066 item 3): `s.trim()`, `s.toUpperCase()`, `s.startsWith(p)`,
// `s.slice(a, b)`, `n.toFixed(d)`. The receiver becomes the first argument of the IR call, so the
// evaluator and codegen see one shape for a function and a method.
ir::ExprPtr lower_string_method(LoweringState& state, const jsx::Expr& call,
                                const jsx::Expr& receiver) {
    const std::string& method = call.operands[0]->text;
    std::vector<ir::ExprPtr> args;
    args.reserve(call.operands.size());
    ir::ExprPtr base = lower_expr(state, receiver);
    if (!base) {
        return nullptr;
    }
    args.push_back(std::move(base));
    for (std::size_t i = 1; i < call.operands.size(); ++i) {
        if (!call.operands[i]) {
            fail(state, kMalformed, "a call argument is missing", call.span);
            return nullptr;
        }
        ir::ExprPtr arg = lower_expr(state, *call.operands[i]);
        if (!arg) {
            return nullptr;
        }
        args.push_back(std::move(arg));
    }
    return ir::make_call(method, std::move(args));
}

} // namespace

ir::ExprPtr lower_call(LoweringState& state, const jsx::Expr& expr) {
    if (expr.operands.empty() || !expr.operands[0]) {
        fail(state, kMalformed, "a call needs a callee", expr.span);
        return nullptr;
    }
    const jsx::Expr& callee = *expr.operands[0];
    // ADR-066 item 3: a method call on a receiver. `filter` and `slice` are the list forms; the
    // total string methods are the rest. Anything else stays rejected.
    if (callee.kind == jsx::ExprKind::Member) {
        if (callee.operands.size() != 1 || !callee.operands[0]) {
            fail(state, kMalformed, "a method call needs a receiver", callee.span);
            return nullptr;
        }
        const jsx::Expr& receiver = *callee.operands[0];
        if (callee.text == "filter") {
            return lower_filter(state, expr, receiver);
        }
        if (callee.text == "slice") {
            return lower_list_slice(state, expr, receiver);
        }
        if (is_string_method(callee.text)) {
            return lower_string_method(state, expr, receiver);
        }
        fail(state, kUnsupported,
             "`." + callee.text +
                 "` is not a pure method; V1 accepts `filter`, `slice`, `trim`, `toUpperCase`, "
                 "`toLowerCase`, `startsWith` and `toFixed` (ir-v1.md section 4.2)",
             callee.span);
        return nullptr;
    }
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
