// SPDX-License-Identifier: MIT

#include "operators.hpp"
#include "tessera/compiler/eval/evaluator.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>

namespace tessera::compiler::eval {
namespace {

using detail::fail;

constexpr std::string_view kState = "state::";
constexpr std::string_view kAnim = "anim::";

std::expected<Value, EvalError> literal_value(const ir::LiteralValue& literal) {
    if (const bool* flag = std::get_if<bool>(&literal)) {
        return Value::of_bool(*flag);
    }
    if (const std::int64_t* integer = std::get_if<std::int64_t>(&literal)) {
        if (*integer < std::numeric_limits<std::int32_t>::min() ||
            *integer > std::numeric_limits<std::int32_t>::max()) {
            return fail("TESSERA_IR_EVAL_LITERAL_RANGE",
                        "the int literal " + std::to_string(*integer) + " does not fit 32 bits");
        }
        return Value::of_int(static_cast<std::int32_t>(*integer));
    }
    if (const double* number = std::get_if<double>(&literal)) {
        return Value::of_float(*number);
    }
    if (const std::string* text = std::get_if<std::string>(&literal)) {
        return Value::of_string(*text);
    }
    return Value::null();
}

bool has_operands(const ir::Expr& expr, std::size_t count) {
    if (expr.operands.size() != count) {
        return false;
    }
    for (const ir::ExprPtr& operand : expr.operands) {
        if (!operand) {
            return false;
        }
    }
    return true;
}

std::unexpected<EvalError> malformed(std::string_view what) {
    return fail("TESSERA_IR_EVAL_MALFORMED", std::string(what));
}

} // namespace

std::expected<Value, EvalError> Evaluator::evaluate(const ir::Expr& expr, const Scope& scope) {
    return eval(expr, scope, 0);
}

void Evaluator::report_modulo_zero(const ir::Expr& expr) {
    if (reported_.insert(&expr).second) {
        diagnostics_.push_back(
            {"TESSERA_IR_INT_MODULO_ZERO", "`%` with a zero int divisor; the result is 0"});
    }
}

std::expected<Value, EvalError> Evaluator::eval(const ir::Expr& expr, const Scope& scope,
                                                std::size_t depth) {
    if (depth > kMaxDepth) {
        return fail("TESSERA_IR_EVAL_DEPTH",
                    "the expression nests deeper than the evaluator allows");
    }
    switch (expr.kind) {
    case ir::ExprKind::Literal:
        return literal_value(expr.literal);
    case ir::ExprKind::Ref: {
        const auto found = scope.find(expr.id);
        if (found != scope.end()) {
            return found->second;
        }
        // A dotted read into a record (ADR-051 slice 5): `state.count` is one ref id, so the
        // longest scope prefix that names a record wins and the rest is a member path. Without
        // this a record would be unusable -- the IR has no separate member-access expression, so
        // the dot is part of the id (ir-v1-expr.md section 4.3, "dotted members").
        const std::size_t dot = expr.id.rfind('.');
        if (dot != std::string::npos) {
            const auto base = scope.find(expr.id.substr(0, dot));
            if (base != scope.end()) {
                if (const Record* record = base->second.as_record()) {
                    const std::string_view path = std::string_view(expr.id).substr(dot + 1);
                    if (const Value* member = record->find(path)) {
                        return *member;
                    }
                    return fail("TESSERA_IR_EVAL_UNKNOWN_REF", "`" + expr.id +
                                                                   "` names no member of record `" +
                                                                   record->type + "`");
                }
            }
        }
        return unresolved_ref(expr, scope);
    }
    case ir::ExprKind::Binary:
        return eval_binary(expr, scope, depth);
    case ir::ExprKind::Unary: {
        if (!has_operands(expr, 1)) {
            return malformed("a unary expression needs one operand");
        }
        auto value = eval(*expr.operands[0], scope, depth + 1);
        if (!value) {
            return value;
        }
        return detail::apply_unary(expr.op, *value);
    }
    case ir::ExprKind::Conditional: {
        if (!has_operands(expr, 3)) {
            return malformed("a conditional needs three operands");
        }
        auto test = eval(*expr.operands[0], scope, depth + 1);
        if (!test) {
            return test;
        }
        const bool* flag = test->as_bool();
        if (flag == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE",
                        "a conditional test must be a bool, got " + std::string(type_name(*test)));
        }
        // Only the chosen branch runs, so the other cannot report a diagnostic or an error.
        return eval(*expr.operands[*flag ? 1 : 2], scope, depth + 1);
    }
    case ir::ExprKind::Template:
        return eval_template(expr, scope, depth);
    case ir::ExprKind::OptionalField: {
        // ADR-066 item 1: `a?.b`. An absent base yields null, so the chain short-circuits; a
        // present base yields the field. The base is a record value here, because the IR has no
        // separate member-access expression (ir-v1-expr.md section 4.3).
        if (!has_operands(expr, 1)) {
            return malformed("an optional field access needs one operand");
        }
        auto base = eval(*expr.operands[0], scope, depth + 1);
        if (!base) {
            return base;
        }
        if (base->is_null()) {
            return Value::null();
        }
        const Record* record = base->as_record();
        if (record == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE", "`?." + expr.id + "` reads a field of a " +
                                                    std::string(type_name(*base)) +
                                                    ", which is not a record");
        }
        const Value* member = record->find(expr.id);
        if (member == nullptr) {
            return fail("TESSERA_IR_EVAL_UNKNOWN_REF",
                        "`" + expr.id + "` names no member of record `" + record->type + "`");
        }
        return *member;
    }
    case ir::ExprKind::Coalesce: {
        // ADR-066 item 1: `a ?? b`. Only the chosen side runs, so the other cannot report a
        // diagnostic or an error, exactly as for a conditional.
        if (!has_operands(expr, 2)) {
            return malformed("a coalesce needs two operands");
        }
        auto value = eval(*expr.operands[0], scope, depth + 1);
        if (!value) {
            return value;
        }
        if (!value->is_null()) {
            return value;
        }
        return eval(*expr.operands[1], scope, depth + 1);
    }
    case ir::ExprKind::ListFilter: {
        // ADR-066 item 3: `list.filter(item => <bool>)`. The predicate runs once per element with
        // the item name bound in a nested scope, so it cannot see or write the outer one.
        if (!has_operands(expr, 2)) {
            return malformed("a list filter needs a list and a predicate");
        }
        auto list = eval(*expr.operands[0], scope, depth + 1);
        if (!list) {
            return list;
        }
        const List* items = list->as_list();
        if (items == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE",
                        "`filter` needs a list, got " + std::string(type_name(*list)));
        }
        List kept;
        for (const Value& item : *items) {
            Scope inner = scope;
            inner[expr.id] = item;
            auto held = eval(*expr.operands[1], inner, depth + 1);
            if (!held) {
                return held;
            }
            const bool* flag = held->as_bool();
            if (flag == nullptr) {
                return fail("TESSERA_IR_EVAL_TYPE", "a `filter` predicate must be a bool, got " +
                                                        std::string(type_name(*held)));
            }
            if (*flag) {
                kept.push_back(item);
            }
        }
        return Value::of_list(std::move(kept));
    }
    case ir::ExprKind::ListSlice: {
        // ADR-066 item 3: `list.slice(start[, end])`. The bounds are clamped to the list, as in
        // JavaScript: a negative or oversized bound is not an error.
        if (expr.operands.size() < 2 || expr.operands.size() > 3) {
            return malformed("a list slice needs a list and one or two bounds");
        }
        auto list = eval(*expr.operands[0], scope, depth + 1);
        if (!list) {
            return list;
        }
        const List* items = list->as_list();
        if (items == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE",
                        "`slice` needs a list, got " + std::string(type_name(*list)));
        }
        const auto bound = [&](std::size_t index,
                               std::int64_t fallback) -> std::expected<std::int64_t, EvalError> {
            if (index >= expr.operands.size()) {
                return fallback;
            }
            auto value = eval(*expr.operands[index], scope, depth + 1);
            if (!value) {
                return std::unexpected(std::move(value.error()));
            }
            const std::int32_t* integer = value->as_int();
            if (integer == nullptr) {
                return fail("TESSERA_IR_EVAL_TYPE", "a `slice` bound must be an int, got " +
                                                        std::string(type_name(*value)));
            }
            return static_cast<std::int64_t>(*integer);
        };
        auto start = bound(1, 0);
        if (!start) {
            return std::unexpected(std::move(start.error()));
        }
        auto end = bound(2, static_cast<std::int64_t>(items->size()));
        if (!end) {
            return std::unexpected(std::move(end.error()));
        }
        const auto size = static_cast<std::int64_t>(items->size());
        const std::int64_t from = std::clamp<std::int64_t>(*start, 0, size);
        const std::int64_t to = std::clamp<std::int64_t>(*end, from, size);
        List kept(items->begin() + from, items->begin() + to);
        return Value::of_list(std::move(kept));
    }
    case ir::ExprKind::RecordUpdate: {
        // ADR-066 item 3: `{ ...record, field: value }`. The base is copied and the written fields
        // replace their members, so the result keeps the record's member order.
        auto base = eval(*ir::make_ref(expr.id), scope, depth + 1);
        if (!base) {
            return base;
        }
        const Record* record = base->as_record();
        if (record == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE", "a record update needs a record base, got " +
                                                    std::string(type_name(*base)));
        }
        Record updated = *record;
        for (const ir::RecordField& field : expr.fields) {
            if (!field.value) {
                return malformed("record update field `" + field.name + "` has no value");
            }
            auto value = eval(*field.value, scope, depth + 1);
            if (!value) {
                return value;
            }
            bool replaced = false;
            for (RecordField& existing : updated.fields) {
                if (existing.name == field.name) {
                    existing.value = std::move(*value);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                return fail("TESSERA_IR_EVAL_UNKNOWN_REF", "`" + field.name +
                                                               "` names no member of record `" +
                                                               record->type + "`");
            }
        }
        return Value::of_record(std::move(updated));
    }
    case ir::ExprKind::Call:
        return eval_call(expr, scope, depth);
    case ir::ExprKind::Array: {
        List items;
        items.reserve(expr.operands.size());
        for (const ir::ExprPtr& operand : expr.operands) {
            if (!operand) {
                return malformed("an array element is missing");
            }
            auto item = eval(*operand, scope, depth + 1);
            if (!item) {
                return item;
            }
            items.push_back(std::move(*item));
        }
        return Value::of_list(std::move(items));
    }
    case ir::ExprKind::Record: {
        // ADR-051 slice 5: a record value evaluates to a `Record` whose fields are in the IR's
        // member order. The evaluator has no type table, so it does not re-check the field set --
        // the IR validator already did that against `types` (ir-v1-expr.md section 4.3) -- and it
        // keeps the declared type name so a dotted read and a diagnostic can both name it.
        Record record;
        record.type = expr.type;
        record.fields.reserve(expr.fields.size());
        for (const ir::RecordField& field : expr.fields) {
            if (!field.value) {
                return malformed("record `" + expr.type + "` field `" + field.name +
                                 "` has no value");
            }
            auto value = eval(*field.value, scope, depth + 1);
            if (!value) {
                return value;
            }
            record.fields.push_back(RecordField{field.name, std::move(*value)});
        }
        return Value::of_record(std::move(record));
    }
    }
    return malformed("unknown expression kind");
}

std::expected<Value, EvalError> Evaluator::eval_binary(const ir::Expr& expr, const Scope& scope,
                                                       std::size_t depth) {
    if (!has_operands(expr, 2)) {
        return malformed("a binary expression needs two operands");
    }
    auto left = eval(*expr.operands[0], scope, depth + 1);
    if (!left) {
        return left;
    }
    if (expr.op == "&&" || expr.op == "||") {
        const bool* lhs = left->as_bool();
        if (lhs == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE",
                        "`" + expr.op + "` needs bools, got " + std::string(type_name(*left)));
        }
        const bool decided = expr.op == "&&" ? !*lhs : *lhs;
        if (decided) {
            return Value::of_bool(*lhs); // short circuit: the right side never runs
        }
        auto right = eval(*expr.operands[1], scope, depth + 1);
        if (!right) {
            return right;
        }
        if (right->as_bool() == nullptr) {
            return fail("TESSERA_IR_EVAL_TYPE",
                        "`" + expr.op + "` needs bools, got " + std::string(type_name(*right)));
        }
        return Value::of_bool(*right->as_bool());
    }
    auto right = eval(*expr.operands[1], scope, depth + 1);
    if (!right) {
        return right;
    }
    bool modulo_zero = false;
    auto result = detail::apply_binary(expr.op, *left, *right, modulo_zero);
    if (modulo_zero) {
        report_modulo_zero(expr);
    }
    return result;
}

std::expected<Value, EvalError> Evaluator::eval_template(const ir::Expr& expr, const Scope& scope,
                                                         std::size_t depth) {
    std::string text;
    for (const ir::TemplatePart& part : expr.parts) {
        if (!part.expr) {
            text += part.text;
            continue;
        }
        auto value = eval(*part.expr, scope, depth + 1);
        if (!value) {
            return value;
        }
        auto piece = detail::template_text(*value);
        if (!piece) {
            return std::unexpected(std::move(piece.error()));
        }
        text += *piece;
    }
    return Value::of_string(std::move(text));
}

std::expected<Value, EvalError> Evaluator::eval_call(const ir::Expr& expr, const Scope& scope,
                                                     std::size_t depth) {
    std::vector<Value> args;
    args.reserve(expr.operands.size());
    for (const ir::ExprPtr& operand : expr.operands) {
        if (!operand) {
            return malformed("a call argument is missing");
        }
        auto value = eval(*operand, scope, depth + 1);
        if (!value) {
            return value;
        }
        args.push_back(std::move(*value));
    }
    return detail::call_function(expr.id, args);
}

std::expected<void, EvalError> Evaluator::run(std::span<const ir::Action> actions, Scope& scope) {
    for (const ir::Action& action : actions) {
        // ADR-066 item 2: an `If` action runs one branch, chosen by its condition. The condition is
        // evaluated here rather than through the generic guard path below, because a false guard
        // there means "skip this action" while here it means "run the else branch".
        if (action.kind == ir::ActionKind::If) {
            if (!action.guard) {
                return fail("TESSERA_IR_EVAL_MALFORMED", "an `if` action has no condition");
            }
            auto held = eval(*action.guard, scope, 0);
            if (!held) {
                return std::unexpected(std::move(held.error()));
            }
            const bool* flag = held->as_bool();
            if (flag == nullptr) {
                return fail("TESSERA_IR_EVAL_TYPE", "an `if` condition must be a bool, got " +
                                                        std::string(type_name(*held)));
            }
            auto branch = run(*flag ? action.then_actions : action.else_actions, scope);
            if (!branch) {
                return branch;
            }
            continue;
        }
        if (action.guard) {
            auto held = eval(*action.guard, scope, 0);
            if (!held) {
                return std::unexpected(std::move(held.error()));
            }
            const bool* flag = held->as_bool();
            if (flag == nullptr) {
                return fail("TESSERA_IR_EVAL_TYPE",
                            "an action guard must be a bool, got " + std::string(type_name(*held)));
            }
            if (!*flag) {
                continue;
            }
        }
        if (is_host_action(action.kind)) {
            // focus, a callback prop and a service call are effects a host models, not values
            if (auto done = run_host_action(action, scope); !done) {
                return done;
            }
            continue;
        }
        if (!action.value) {
            return fail("TESSERA_IR_EVAL_MALFORMED", "an action has no value expression");
        }
        const bool state_target =
            action.kind == ir::ActionKind::Set && action.target.rfind(kState, 0) == 0;
        const bool anim_target =
            action.kind == ir::ActionKind::AnimSet && action.target.rfind(kAnim, 0) == 0;
        if (!state_target && !anim_target) {
            return fail("TESSERA_IR_EVAL_ACTION_TARGET",
                        "`" + action.target + "` is not a valid target for this action");
        }
        const auto target = scope.find(action.target);
        if (target == scope.end()) {
            return fail("TESSERA_IR_EVAL_UNKNOWN_REF",
                        "`" + action.target + "` is not declared in scope");
        }
        auto value = eval(*action.value, scope, 0);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        // ADR-067 item 4: a transition-marked setter starts a transition before it assigns, so
        // `isPending` is true from the moment the change is applied. Inert without a session,
        // which is the default.
        if (action.transition && pending_ != nullptr) {
            start_transition(action.target, scope);
        }
        // The target keeps its type; an int widens into a float as C++ converts it.
        if (target->second.as_float() != nullptr && value->as_int() != nullptr) {
            *value = Value::of_float(value->number());
        }
        if (target->second.data.index() != value->data.index()) {
            return fail("TESSERA_IR_EVAL_TYPE", "`" + action.target + "` holds a " +
                                                    std::string(type_name(target->second)) +
                                                    ", cannot assign a " +
                                                    std::string(type_name(*value)));
        }
        target->second = std::move(*value);
    }
    return {};
}

} // namespace tessera::compiler::eval
