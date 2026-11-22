// SPDX-License-Identifier: MIT

// Expression, action, type and span checks (docs/specs/ir-v1.md sections 4, 8 and 9).

#include "validate_internal.hpp"
#include "validate_record_detail.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>
#include <utility>

namespace tessera::compiler::ir {
namespace {

constexpr std::string_view kBinaryOps[] = {
    "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||"};
constexpr std::string_view kUnaryOps[] = {"!", "-", "+"};
// ir-v1.md section 4.2. The last six are the total string methods of ADR-066 item 3; each takes its
// receiver as the first argument, so a method call and a function call are one shape.
constexpr std::string_view kFunctions[] = {
    "min",         "max",         "clamp",      "abs",    "floor",        "ceil",
    "round",       "toInt",       "toFloat",    "length", "formatNumber", "trim",
    "toUpperCase", "toLowerCase", "startsWith", "slice",  "toFixed"};
constexpr std::string_view kPrimitiveTypes[] = {"bool",  "int",    "float",    "string",
                                                "color", "length", "node_ref", "children"};

template<std::size_t N> bool contains(const std::string_view (&list)[N], std::string_view value) {
    return std::find(std::begin(list), std::end(list), value) != std::end(list);
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool is_local(const LocalNames& locals, std::string_view name) {
    return std::find(locals.begin(), locals.end(), name) != locals.end();
}

bool valid_type(std::string_view type) {
    if (type.empty()) {
        return false;
    }
    if (contains(kPrimitiveTypes, type)) {
        return true;
    }
    for (const std::string_view wrapper : {"list<", "optional<"}) {
        if (starts_with(type, wrapper) && type.back() == '>') {
            return valid_type(type.substr(wrapper.size(), type.size() - wrapper.size() - 1));
        }
    }
    // An identifier: a declared record or a payload record name. Which one is section 2.2's type
    // table check (`validate_type_names`), not a shape rule.
    const auto word = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    };
    return !(type.front() >= '0' && type.front() <= '9') &&
           std::all_of(type.begin(), type.end(), word);
}

} // namespace

// A reference id is a scoped id (`state::x`), a prop (`prop::x`) or a local name, each optionally
// followed by `.field` for a record field.
bool reference_resolves(const ComponentScope& scope, const LocalNames& locals,
                        std::string_view id) {
    const std::string base(id.substr(0, id.find('.')));
    if (starts_with(base, "prop::")) {
        return scope.props.count(base.substr(6)) != 0;
    }
    if (starts_with(base, "state::") || starts_with(base, "ref::") || starts_with(base, "anim::") ||
        starts_with(base, "env::") || starts_with(base, "beh::")) {
        return scope.ids.count(base) != 0;
    }
    return is_local(locals, base);
}

void ValidationContext::report(std::string code, std::string message, const SourceSpan& span) {
    diagnostics.push_back(IrDiagnostic{std::move(code), std::move(message), span});
}

void require_expr(ValidationContext& context, const ComponentScope& scope, const LocalNames& locals,
                  const Expr* expr, const SourceSpan& where, const std::string& what,
                  bool record_value_allowed) {
    if (expr == nullptr) {
        context.report("TESSERA_IR_MALFORMED", what + " is missing", where);
        return;
    }
    validate_expr(context, scope, locals, expr, where, record_value_allowed);
}

void validate_expr(ValidationContext& context, const ComponentScope& scope,
                   const LocalNames& locals, const Expr* expr, const SourceSpan& where,
                   bool record_value_allowed) {
    if (expr == nullptr) {
        return;
    }
    const auto operands = [&](std::size_t count, const char* what) {
        if (expr->operands.size() != count) {
            context.report("TESSERA_IR_MALFORMED",
                           std::string(what) + " needs " + std::to_string(count) + " operand(s)",
                           where);
        }
        for (const auto& operand : expr->operands) {
            require_expr(context, scope, locals, operand.get(), where, "an operand");
        }
    };
    switch (expr->kind) {
    case ExprKind::Literal:
        if (const auto* number = std::get_if<double>(&expr->literal);
            number != nullptr && !std::isfinite(*number)) {
            context.report("TESSERA_IR_LITERAL", "a float literal must be finite", where);
        }
        break;
    case ExprKind::Ref:
        if (!reference_resolves(scope, locals, expr->id)) {
            context.report("TESSERA_IR_UNRESOLVED", "unresolved reference `" + expr->id + "`",
                           where);
        } else {
            // Section 4.3: a dotted ref into a declared record must name a member it has.
            validate_reference_member(context, scope, expr->id, where);
        }
        break;
    case ExprKind::Binary:
        if (!contains(kBinaryOps, expr->op)) {
            context.report("TESSERA_IR_UNSUPPORTED",
                           "unsupported binary operator `" + expr->op + "`", where);
        }
        operands(2, "a binary expression");
        break;
    case ExprKind::Unary:
        if (!contains(kUnaryOps, expr->op)) {
            context.report("TESSERA_IR_UNSUPPORTED",
                           "unsupported unary operator `" + expr->op + "`", where);
        }
        operands(1, "a unary expression");
        break;
    case ExprKind::Conditional:
        operands(3, "a conditional");
        break;
    case ExprKind::Template:
        for (const auto& part : expr->parts) {
            if (part.expr) {
                validate_expr(context, scope, locals, part.expr.get(), where);
            }
        }
        break;
    case ExprKind::Call:
        if (!contains(kFunctions, expr->id)) {
            context.report("TESSERA_IR_UNSUPPORTED", "unsupported function `" + expr->id + "`",
                           where);
        }
        for (const auto& arg : expr->operands) {
            require_expr(context, scope, locals, arg.get(), where, "an argument");
        }
        break;
    case ExprKind::Array:
        // An array literal (section 4): one operand per element. The element expressions are
        // validated like any other; lowering is what restricts them to literals where the subset
        // spec does (docs/specs/jsx-subset-v1.md section 8.4).
        for (const auto& item : expr->operands) {
            require_expr(context, scope, locals, item.get(), where, "an array element");
        }
        break;
    case ExprKind::Record:
        // Section 4.3: a record value is allowed only as a `useState` initial, a prop default or a
        // field of another record value. Elsewhere it is a hard error and only its members would
        // take part, so the field values are not validated again here.
        if (is_predeclared_record(expr->type)) {
            // A payload record is framework data: it has no aggregate form anywhere.
            context.report("TESSERA_IR_UNSUPPORTED",
                           "the framework payload record `" + expr->type +
                               "` is not a project record and has no record value",
                           where);
            break;
        }
        if (!record_value_allowed) {
            context.report("TESSERA_IR_RECORD_VALUE",
                           "a record value is only a `useState` initial, a prop default or a field "
                           "of another record value (section 4.3)",
                           where);
            break;
        }
        validate_record_expr(context, scope, locals, *expr, nullptr, where);
        break;
    case ExprKind::OptionalField:
        // ADR-066 item 1: `a?.b`. One operand (the optional base) and a field name.
        if (expr->id.empty()) {
            context.report("TESSERA_IR_MALFORMED", "an optional field access needs a field name",
                           where);
        }
        operands(1, "an optional field access");
        break;
    case ExprKind::Coalesce:
        // ADR-066 item 1: `a ?? b`. Two operands: the optional value and the fallback.
        operands(2, "a coalesce");
        break;
    case ExprKind::ListFilter:
        // ADR-066 item 3: `list.filter(item => <bool>)`. The item name is what the predicate binds,
        // so an empty one has nothing to bind.
        if (expr->id.empty()) {
            context.report("TESSERA_IR_MALFORMED", "a list filter needs an item name", where);
        }
        operands(2, "a list filter");
        break;
    case ExprKind::ListSlice:
        // ADR-066 item 3: `list.slice(start[, end])`. One or two bounds, and the list first.
        if (expr->operands.size() < 2 || expr->operands.size() > 3) {
            context.report("TESSERA_IR_MALFORMED",
                           "a list slice needs a list and one or two bounds", where);
        }
        for (const auto& operand : expr->operands) {
            require_expr(context, scope, locals, operand.get(), where, "a slice operand");
        }
        break;
    case ExprKind::RecordUpdate:
        // ADR-066 item 3: `{ ...record, field: value }`. The base is a ref id, so it must resolve;
        // the fields are checked against the record type like a record value's.
        if (expr->type.empty() || expr->id.empty()) {
            context.report("TESSERA_IR_MALFORMED",
                           "a record update needs a type and a base reference", where);
            break;
        }
        if (!reference_resolves(scope, locals, expr->id)) {
            context.report("TESSERA_IR_UNRESOLVED",
                           "record update base `" + expr->id + "` does not resolve", where);
        }
        validate_record_expr(context, scope, locals, *expr, nullptr, where);
        break;
    }
}

void validate_type(ValidationContext& context, const std::string& type, const SourceSpan& where) {
    validate_type(context, type, where, true);
}

void validate_type(ValidationContext& context, const std::string& type, const SourceSpan& where,
                   bool mark_used) {
    if (!valid_type(type)) {
        context.report("TESSERA_IR_UNSUPPORTED", "unsupported type `" + type + "`", where);
        return;
    }
    // Section 2.2: the shape is fine, so the identifier it names must be in the table. This is the
    // rule that replaces "any identifier is a record type" (the old section 11 limitation).
    validate_type_names(context, type, where, mark_used);
}

void validate_span(ValidationContext& context, const SourceSpan& span, const std::string& what) {
    const bool ordered =
        span.end_line > span.line || (span.end_line == span.line && span.end_column >= span.column);
    if (span.file.empty() || span.line == 0 || span.column == 0 || !ordered) {
        context.report("TESSERA_IR_SPAN",
                       what + " needs a source span with a file and a start " + "not after its end",
                       span);
    }
}

} // namespace tessera::compiler::ir
