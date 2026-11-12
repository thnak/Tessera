// SPDX-License-Identifier: MIT

// Expression, action, type and span checks (docs/specs/ir-v1.md sections 4, 8 and 9).

#include "validate_internal.hpp"

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
// ir-v1.md section 4.2.
constexpr std::string_view kFunctions[] = {"min",     "max",    "clamp",       "abs",
                                           "floor",   "ceil",   "round",       "toInt",
                                           "toFloat", "length", "formatNumber"};
constexpr std::string_view kPrimitiveTypes[] = {"bool",  "int",    "float",    "string",
                                                "color", "length", "node_ref", "children"};

constexpr std::string_view kEasingKeywords[] = {"linear", "ease", "ease-in", "ease-out",
                                                "ease-in-out"};
constexpr double kMaxAnimationMs = 60'000.0;

// `cubic-bezier(x1, y1, x2, y2)` with the x control points in [0, 1] (the CSS rule that keeps the
// curve a function of time).
bool valid_cubic_bezier(std::string_view text) {
    constexpr std::string_view kPrefix = "cubic-bezier(";
    if (!text.starts_with(kPrefix) || !text.ends_with(')')) {
        return false;
    }
    std::string body(text.substr(kPrefix.size(), text.size() - kPrefix.size() - 1));
    double values[4] = {};
    std::size_t start = 0;
    for (int index = 0; index < 4; ++index) {
        const std::size_t end = index == 3 ? body.size() : body.find(',', start);
        if (end == std::string::npos || end == start) {
            return false;
        }
        const std::string piece = body.substr(start, end - start);
        std::size_t used = 0;
        try {
            values[index] = std::stod(piece, &used);
        } catch (...) {
            return false;
        }
        while (used < piece.size() && piece[used] == ' ') {
            ++used;
        }
        if (used != piece.size() || !std::isfinite(values[index])) {
            return false;
        }
        start = end + 1;
    }
    return values[0] >= 0.0 && values[0] <= 1.0 && values[2] >= 0.0 && values[2] <= 1.0;
}

bool valid_easing(std::string_view text) {
    return std::find(std::begin(kEasingKeywords), std::end(kEasingKeywords), text) !=
               std::end(kEasingKeywords) ||
           valid_cubic_bezier(text);
}

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
    // A record type declared in the source (decision 8 of docs/design/jsx-ir-lowering.md).
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
        starts_with(base, "env::")) {
        return scope.ids.count(base) != 0;
    }
    return is_local(locals, base);
}

void ValidationContext::report(std::string code, std::string message, const SourceSpan& span) {
    diagnostics.push_back(IrDiagnostic{std::move(code), std::move(message), span});
}

void require_expr(ValidationContext& context, const ComponentScope& scope, const LocalNames& locals,
                  const Expr* expr, const SourceSpan& where, const std::string& what) {
    if (expr == nullptr) {
        context.report("TESSERA_IR_MALFORMED", what + " is missing", where);
        return;
    }
    validate_expr(context, scope, locals, expr, where);
}

void validate_expr(ValidationContext& context, const ComponentScope& scope,
                   const LocalNames& locals, const Expr* expr, const SourceSpan& where) {
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
    }
}

bool in_range(double value, double low, double high, bool low_inclusive) {
    return std::isfinite(value) && value <= high && (low_inclusive ? value >= low : value > low);
}

void validate_spring(ValidationContext& context, const SpringTiming& spring, const Action& action,
                     const SourceSpan& where) {
    if (action.duration_ms.has_value() || !action.easing.empty()) {
        context.report("TESSERA_IR_ANIMATION", "a spring cannot take a duration or easing", where);
    }
    if (!in_range(spring.stiffness, 0.0, 100000.0, false)) {
        context.report("TESSERA_IR_ANIMATION", "spring stiffness must be in (0, 100000]", where);
    }
    if (!in_range(spring.damping, 0.0, 10000.0, true)) {
        context.report("TESSERA_IR_ANIMATION", "spring damping must be in [0, 10000]", where);
    }
    if (!in_range(spring.mass, 0.0, 1000.0, false)) {
        context.report("TESSERA_IR_ANIMATION", "spring mass must be in (0, 1000]", where);
    }
}

// Timing options exist on `animSet` only, are meaningless when `animate` is false, and are bounded
// so a generated tween always finishes (ir-v1.md section 2.1).
void validate_animation_options(ValidationContext& context, const Action& action,
                                const SourceSpan& where) {
    const bool has_timing =
        action.duration_ms.has_value() || !action.easing.empty() || action.spring.has_value();
    if (!has_timing) {
        return;
    }
    if (action.kind != ActionKind::AnimSet) {
        context.report("TESSERA_IR_ANIMATION",
                       "only an `animSet` action takes duration, easing or spring", where);
        return;
    }
    if (!action.animate) {
        context.report("TESSERA_IR_ANIMATION",
                       "`animate: false` snaps, so it cannot take a duration, easing or spring",
                       where);
    }
    if (action.spring.has_value()) {
        validate_spring(context, *action.spring, action, where);
    }
    if (action.duration_ms.has_value() &&
        (!std::isfinite(*action.duration_ms) || *action.duration_ms < 0.0 ||
         *action.duration_ms > kMaxAnimationMs)) {
        context.report("TESSERA_IR_ANIMATION", "duration must be 0..60000 ms", where);
    }
    if (!action.easing.empty() && !valid_easing(action.easing)) {
        context.report("TESSERA_IR_ANIMATION",
                       "easing must be linear, ease, ease-in, ease-out, ease-in-out or "
                       "cubic-bezier(x1, y1, x2, y2) with x in [0, 1]",
                       where);
    }
}

void validate_actions(ValidationContext& context, const ComponentScope& scope,
                      const LocalNames& locals, const std::vector<Action>& actions,
                      const SourceSpan& where) {
    for (const auto& action : actions) {
        const char* prefix = action.kind == ActionKind::Set ? "state::" : "anim::";
        if (!starts_with(action.target, prefix) || scope.ids.count(action.target) == 0) {
            context.report(
                "TESSERA_IR_UNRESOLVED",
                "action target `" + action.target + "` is not a " +
                    (action.kind == ActionKind::Set ? "state variable" : "animated value"),
                where);
        }
        require_expr(context, scope, locals, action.value.get(), where, "an action value");
        validate_animation_options(context, action, where);
    }
}

void validate_type(ValidationContext& context, const std::string& type, const SourceSpan& where) {
    if (!valid_type(type)) {
        context.report("TESSERA_IR_UNSUPPORTED", "unsupported type `" + type + "`", where);
    }
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
