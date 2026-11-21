// SPDX-License-Identifier: MIT

// IR expressions -> C++ source text (docs/design/codegen.md "Expression emission").
//
// Every kind that has no correct C++ spelling yet is rejected with TESSERA_CODEGEN_UNSUPPORTED
// instead of emitting code that does not compile or silently changes semantics. The type table
// (compiler slice 4) is what will lift the deferred kinds (`%`, mixed record fields, text
// templates with substitutions).
//
// Naming and literal spelling live in `emit_expr_names.cpp`, and the action emitter in
// `emit_expr_actions.cpp` (issue #327: this file had grown past the 500-line hard limit).

#include "codegen_internal.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::compiler::codegen::internal {
namespace {

ExprEmission fail(std::string message) {
    return ExprEmission{"", std::move(message)};
}

} // namespace

const LocalBinding* find_local(const LocalBindings& locals, std::string_view id) {
    for (const LocalBinding& local : locals) {
        if (local.name == id) {
            return &local;
        }
    }
    return nullptr;
}

ExprEmission emit_expr(const ir::Expr& expr, const ir::Component& component,
                       const LocalBindings& locals) {
    (void)component;
    switch (expr.kind) {
    case ir::ExprKind::Literal:
        return ExprEmission{literal_text(expr.literal), {}};
    case ir::ExprKind::Ref: {
        if (const LocalBinding* local = find_local(locals, expr.id)) {
            return ExprEmission{local->cpp, {}};
        }
        // A behavior output (ADR-064 item 7, behavior-hooks-v1.md section 5): `beh::<name>.<field>`
        // reads the generated accessor `<member><field>()`, which returns the manifest default
        // before the first attach and after detach. The dotted form is the only valid shape, so a
        // bare `beh::<name>` is an error rather than a member read.
        if (expr.id.rfind("beh::", 0) == 0) {
            const std::size_t dot = expr.id.find('.');
            if (dot == std::string::npos) {
                return fail("behavior `" + expr.id +
                            "` is a record; read one of its outputs (`" + expr.id + ".<field>`)");
            }
            const std::string base = expr.id.substr(0, dot);
            const std::string field = expr.id.substr(dot + 1);
            return ExprEmission{member_name(base) + field + "()", {}};
        }
        if (const std::size_t dot = expr.id.find('.'); dot != std::string::npos) {
            if (find_local(locals, std::string_view(expr.id).substr(0, dot)) != nullptr) {
                return fail("`" + expr.id +
                            "` reads a field of a loop item; record element types have no C++ form "
                            "in this codegen slice (list element types are primitives only)");
            }
            return fail("record field `" + expr.id +
                        "` needs the type table (slice 4); unsupported in this codegen slice");
        }
        const std::size_t split = expr.id.find("::");
        if (split == std::string::npos || !member_prefix(expr.id.substr(0, split + 2))) {
            return fail("unsupported reference `" + expr.id +
                        "`; a local is only visible inside the list template that binds it");
        }
        return ExprEmission{member_name(expr.id), {}};
    }
    case ir::ExprKind::Binary: {
        if (expr.op == "%") {
            return fail("`%` needs the type table (int vs float remainder), deferred");
        }
        if (expr.operands.size() != 2 || !expr.operands[0] || !expr.operands[1]) {
            return fail("a binary expression needs two operands");
        }
        if (expr.op == "==" || expr.op == "!=") {
            // `e.key == "Escape"` (issue #236): the C++ payload carries a key code, not the W3C
            // string, so the comparison goes through `key_is`.
            for (std::size_t side = 0; side < 2; ++side) {
                const ir::Expr& key = *expr.operands[side];
                const ir::Expr& name = *expr.operands[1 - side];
                const LocalBinding* local =
                    key.kind == ir::ExprKind::Ref ? find_local(locals, key.id) : nullptr;
                if (local == nullptr || local->ir_type != "KeyName") {
                    continue;
                }
                const auto* text = name.kind == ir::ExprKind::Literal
                                       ? std::get_if<std::string>(&name.literal)
                                       : nullptr;
                if (text == nullptr) {
                    return fail("`" + key.id + "` can only be compared with a string literal");
                }
                return ExprEmission{std::string(expr.op == "!=" ? "(!" : "(") +
                                        "tessera::ui::key_is(" + local->cpp + ", " +
                                        escape_string(*text) + "))",
                                    {}};
            }
        }
        ExprEmission left = emit_expr(*expr.operands[0], component, locals);
        if (!left.error.empty()) {
            return left;
        }
        ExprEmission right = emit_expr(*expr.operands[1], component, locals);
        if (!right.error.empty()) {
            return right;
        }
        if (expr.op == "/") {
            // IR `/` always yields float (ir-v1 section 4.1); C++ integer division would truncate.
            return ExprEmission{"(static_cast<double>(" + left.text + ") / static_cast<double>(" +
                                    right.text + "))",
                                {}};
        }
        return ExprEmission{"(" + left.text + " " + expr.op + " " + right.text + ")", {}};
    }
    case ir::ExprKind::Unary: {
        if (expr.operands.size() != 1 || !expr.operands[0]) {
            return fail("a unary expression needs one operand");
        }
        ExprEmission value = emit_expr(*expr.operands[0], component, locals);
        if (!value.error.empty()) {
            return value;
        }
        return ExprEmission{"(" + expr.op + value.text + ")", {}};
    }
    case ir::ExprKind::Conditional: {
        if (expr.operands.size() != 3 || !expr.operands[0] || !expr.operands[1] ||
            !expr.operands[2]) {
            return fail("a conditional needs three operands");
        }
        ExprEmission test = emit_expr(*expr.operands[0], component, locals);
        ExprEmission then_value = emit_expr(*expr.operands[1], component, locals);
        ExprEmission else_value = emit_expr(*expr.operands[2], component, locals);
        if (!test.error.empty()) {
            return test;
        }
        if (!then_value.error.empty()) {
            return then_value;
        }
        if (!else_value.error.empty()) {
            return else_value;
        }
        return ExprEmission{
            "(" + test.text + " ? " + then_value.text + " : " + else_value.text + ")", {}};
    }
    case ir::ExprKind::Template: {
        std::string folded;
        for (const ir::TemplatePart& part : expr.parts) {
            if (part.expr) {
                return fail("a text template with substitutions needs the text runtime, deferred");
            }
            folded += part.text;
        }
        return ExprEmission{escape_string(folded), {}};
    }
    case ir::ExprKind::Call: {
        static constexpr std::pair<std::string_view, std::string_view> kCalls[] = {
            {"min", "std::min"},     {"max", "std::max"},     {"clamp", "std::clamp"},
            {"abs", "std::abs"},     {"floor", "std::floor"}, {"ceil", "std::ceil"},
            {"round", "std::round"},
        };
        if (expr.id == "length" || expr.id == "formatNumber") {
            return fail("`" + expr.id + "` needs the string/format runtime, deferred");
        }
        if (expr.id == "toInt" || expr.id == "toFloat") {
            if (expr.operands.size() != 1 || !expr.operands[0]) {
                return fail("`" + expr.id + "` takes one argument");
            }
            ExprEmission value = emit_expr(*expr.operands[0], component, locals);
            if (!value.error.empty()) {
                return value;
            }
            return ExprEmission{
                (expr.id == "toInt" ? "static_cast<int>(" : "static_cast<double>(") + value.text +
                    ")",
                {}};
        }
        std::string_view callee;
        for (const auto& [name, cpp] : kCalls) {
            if (expr.id == name) {
                callee = cpp;
                break;
            }
        }
        if (callee.empty()) {
            return fail("unsupported function `" + expr.id + "`; no C++ spelling in this slice");
        }
        std::string text = std::string(callee) + "(";
        for (std::size_t i = 0; i < expr.operands.size(); ++i) {
            if (!expr.operands[i]) {
                return fail("a call argument is missing");
            }
            ExprEmission arg = emit_expr(*expr.operands[i], component, locals);
            if (!arg.error.empty()) {
                return arg;
            }
            if (i != 0) {
                text += ", ";
            }
            text += arg.text;
        }
        text += ")";
        return ExprEmission{std::move(text), {}};
    }
    case ir::ExprKind::Array:
        // An array literal is only a state/prop initial value (docs/specs/ir-v1.md section 4); the
        // initializer position has its own emitter so a braced list never leaks into an expression.
        return fail("an array literal has no C++ spelling in an expression position; it is only a "
                    "state or prop initial value");
    case ir::ExprKind::Record: {
        // ADR-051 section 5: a record value is designated aggregate initialization
        // (`Size{.w = 1, .h = 2}`), so the member names and their order are compiler-checked
        // against the generated struct. A record value only reaches an initializer position
        // (section 4.3).
        std::string text = expr.type + "{";
        for (std::size_t i = 0; i < expr.fields.size(); ++i) {
            const ir::RecordField& field = expr.fields[i];
            if (!field.value) {
                return fail("record `" + expr.type + "` field `" + field.name + "` has no value");
            }
            const ExprEmission value = emit_initializer_expr(*field.value, component);
            if (!value.error.empty()) {
                return value;
            }
            if (i != 0) {
                text += ", ";
            }
            text += "." + field.name + " = " + value.text;
        }
        text += "}";
        return ExprEmission{std::move(text), {}};
    }
    }
    return fail("unsupported expression kind");
}

ExprEmission emit_initializer_expr(const ir::Expr& expr, const ir::Component& component) {
    if (expr.kind != ir::ExprKind::Array) {
        // A record value is an initializer form too (`Size{.w = 1}`), never a bare expression.
        return emit_expr(expr, component);
    }
    std::string text = "{";
    for (std::size_t i = 0; i < expr.operands.size(); ++i) {
        if (!expr.operands[i]) {
            return fail("an array initial has a missing element");
        }
        const ExprEmission element = emit_expr(*expr.operands[i], component);
        if (!element.error.empty()) {
            return element;
        }
        if (i != 0) {
            text += ", ";
        }
        text += element.text;
    }
    text += "}";
    return ExprEmission{std::move(text), {}};
}

} // namespace tessera::compiler::codegen::internal
