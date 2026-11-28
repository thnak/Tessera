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
#include "value_ops.hpp"

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
        // A resource (ADR-067 item 1, pending-ui-v1.md section 3.4): `res::<name>.status` reads the
        // generated accessor over the resource's view. `.error` is the failure text (a string);
        // `.data` is generated only for a typed resource (the IR carries the result type in
        // `type`).
        if (expr.id.rfind("res::", 0) == 0) {
            const std::size_t dot = expr.id.find('.');
            const std::string member = dot == std::string::npos ? "" : expr.id.substr(dot + 1);
            std::string stem = member_name(expr.id.substr(0, dot));
            stem.pop_back();
            if (member == "status") {
                return ExprEmission{"std::string(" + stem + "_status())", {}};
            }
            // `.error` is the text of the last failure, a string for every resource.
            if (member == "error") {
                return ExprEmission{"std::string(" + stem + "_state().error)", {}};
            }
            // `.data` of a typed resource (Resource::type): the scalar the view publishes, the zero
            // value while absent. Without a type there is nothing to emit.
            if (member == "data") {
                for (const ir::Resource& resource : component.resources) {
                    if (resource.id != expr.id.substr(0, dot) || resource.type.empty()) {
                        continue;
                    }
                    if (resource.type == "string") {
                        return ExprEmission{"std::string(" + stem + "_state().text)", {}};
                    }
                    if (resource.type == "bool") {
                        return ExprEmission{"(" + stem + "_state().number != 0.0)", {}};
                    }
                    if (resource.type == "int") {
                        return ExprEmission{
                            "static_cast<std::int64_t>(" + stem + "_state().number)", {}};
                    }
                    return ExprEmission{stem + "_state().number", {}};
                }
            }
            return fail("`" + expr.id +
                        "` is not generated: `.data` needs the resource's `type` (the service "
                        "method's declared return type)");
        }
        // A service observable (ADR-050 open question 6): `obs::<name>` reads the generated value
        // accessor, which returns the observable's live value (the zero value before attach), and
        // `obs::<name>.<field>` reads a field of a record-typed one.
        if (expr.id.rfind("obs::", 0) == 0) {
            const std::size_t dot = expr.id.find('.');
            std::string stem = member_name(expr.id.substr(0, dot));
            return ExprEmission{
                stem + "value()" + (dot == std::string::npos ? "" : expr.id.substr(dot)), {}};
        }
        // A behavior output (ADR-064 item 7, behavior-hooks-v1.md section 5): `beh::<name>.<field>`
        // reads the generated accessor `<member><field>()`, which returns the manifest default
        // before the first attach and after detach. The dotted form is the only valid shape, so a
        // bare `beh::<name>` is an error rather than a member read.
        if (expr.id.rfind("beh::", 0) == 0) {
            const std::size_t dot = expr.id.find('.');
            if (dot == std::string::npos) {
                return fail("behavior `" + expr.id + "` is a record; read one of its outputs (`" +
                            expr.id + ".<field>`)");
            }
            const std::string base = expr.id.substr(0, dot);
            const std::string field = expr.id.substr(dot + 1);
            return ExprEmission{member_name(base) + field + "()", {}};
        }
        if (const std::size_t dot = expr.id.find('.'); dot != std::string::npos) {
            // A dotted read of a loop item's record field (`row.name`): the loop local's C++ name
            // plus the field path is exactly the aggregate member the item scope reads. The IR has
            // no member-access expression, so the dot is part of the id, as for a record cell. A
            // primitive element type (a type with a printable form) has no field, so it stays an
            // error.
            const std::string_view local = std::string_view(expr.id).substr(0, dot);
            if (const LocalBinding* binding = find_local(locals, local); binding != nullptr) {
                if (text_value_type(binding->ir_type).has_value()) {
                    return fail("`" + expr.id + "` reads a field of a primitive loop item");
                }
                return ExprEmission{binding->cpp + std::string(expr.id.substr(dot)), {}};
            }
            // ADR-066 item 4: a dotted read into a record-typed cell. The IR has no member-access
            // expression, so the dot is part of the id (ir-v1-expr.md section 4.3, "dotted
            // members"): the generated member name of the base plus the field path is exactly the
            // C++ the generated aggregate uses, so `state::state.count` is `state_state_.count`.
            const std::string_view base = std::string_view(expr.id).substr(0, dot);
            const std::size_t split = base.find("::");
            if (split == std::string_view::npos || !member_prefix(base.substr(0, split + 2))) {
                return fail("record field `" + expr.id + "` does not name a declared cell or prop");
            }
            return ExprEmission{member_name(base) + std::string(expr.id.substr(dot)), {}};
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
    case ir::ExprKind::OptionalField: {
        // ADR-066 item 1: `a?.b` over an `optional<T>`. The generated C++ is the same shape the
        // evaluator implements: an absent base yields an empty optional, a present one yields the
        // field. `std::optional` has no member access, so the presence test is written out.
        if (expr.operands.size() != 1 || !expr.operands[0]) {
            return fail("an optional field access needs one operand");
        }
        ExprEmission base = emit_expr(*expr.operands[0], component, locals);
        if (!base.error.empty()) {
            return base;
        }
        const std::string text =
            "(" + base.text + ".has_value() ? " + base.text + "->" + expr.id + " : std::nullopt)";
        return ExprEmission{text, {}};
    }
    case ir::ExprKind::Coalesce: {
        // ADR-066 item 1: `a ?? b`. `value_or` is the direct spelling, but it evaluates the
        // fallback eagerly; the conditional keeps the short-circuit the evaluator has, so a
        // fallback with a side effect (a call) behaves the same in both.
        if (expr.operands.size() != 2 || !expr.operands[0] || !expr.operands[1]) {
            return fail("a coalesce needs two operands");
        }
        ExprEmission value = emit_expr(*expr.operands[0], component, locals);
        if (!value.error.empty()) {
            return value;
        }
        ExprEmission fallback = emit_expr(*expr.operands[1], component, locals);
        if (!fallback.error.empty()) {
            return fallback;
        }
        const std::string text =
            "(" + value.text + ".has_value() ? *" + value.text + " : " + fallback.text + ")";
        return ExprEmission{text, {}};
    }
    case ir::ExprKind::ListFilter: {
        // ADR-066 item 3: `list.filter(item => <bool>)`. The generated C++ is a lambda over the
        // list, so the predicate's item name is a real local and the result is a new vector.
        if (expr.operands.size() != 2 || !expr.operands[0] || !expr.operands[1]) {
            return fail("a list filter needs a list and a predicate");
        }
        ExprEmission list = emit_expr(*expr.operands[0], component, locals);
        if (!list.error.empty()) {
            return list;
        }
        LocalBindings inner = locals;
        // The predicate's item name is a real C++ local, so it is bound for the predicate's
        // emission and shadows any outer name of the same spelling.
        inner.push_back(LocalBinding{expr.id, expr.id, "string"});
        ExprEmission predicate = emit_expr(*expr.operands[1], component, inner);
        if (!predicate.error.empty()) {
            return predicate;
        }
        // The predicate may read a generated member (`prop_query_`), so the lambda captures
        // `this` by value; a predicate that reads only its item still compiles with the capture.
        const std::string text = "tessera::ui::filter_list(" + list.text + ", [this](const auto& " +
                                 expr.id + ") { return " + predicate.text + "; })";
        return ExprEmission{text, {}};
    }
    case ir::ExprKind::ListSlice: {
        // ADR-066 item 3: `list.slice(start[, end])`. `slice_list` clamps the bounds, so the
        // generated code and the evaluator agree on an out-of-range bound.
        if (expr.operands.size() < 2 || expr.operands.size() > 3 || !expr.operands[0]) {
            return fail("a list slice needs a list and one or two bounds");
        }
        ExprEmission list = emit_expr(*expr.operands[0], component, locals);
        if (!list.error.empty()) {
            return list;
        }
        std::string text = "tessera::ui::slice_list(" + list.text;
        for (std::size_t i = 1; i < expr.operands.size(); ++i) {
            if (!expr.operands[i]) {
                return fail("a list slice bound is missing");
            }
            ExprEmission bound = emit_expr(*expr.operands[i], component, locals);
            if (!bound.error.empty()) {
                return bound;
            }
            text += ", " + bound.text;
        }
        text += ")";
        return ExprEmission{std::move(text), {}};
    }
    case ir::ExprKind::RecordUpdate: {
        // ADR-066 item 3: `{ ...record, field: value }`. The generated C++ copies the base and
        // assigns the written members, so the result keeps the record's member order.
        if (expr.id.empty()) {
            return fail("a record update needs a base reference");
        }
        ExprEmission base = emit_expr(*ir::make_ref(expr.id), component, locals);
        if (!base.error.empty()) {
            return base;
        }
        std::string text = "([&] { auto updated = " + base.text + ";";
        for (const ir::RecordField& field : expr.fields) {
            if (!field.value) {
                return fail("a record update field is missing its value");
            }
            ExprEmission value = emit_expr(*field.value, component, locals);
            if (!value.error.empty()) {
                return value;
            }
            text += " updated." + field.name + " = " + value.text + ";";
        }
        text += " return updated; }())";
        return ExprEmission{std::move(text), {}};
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
        // ADR-066 item 3: the total string methods, emitted as the same helpers the evaluator uses
        // so the two agree byte for byte. Each takes its receiver as the first argument.
        if (expr.id == "trim" || expr.id == "toUpperCase" || expr.id == "toLowerCase" ||
            expr.id == "startsWith" || expr.id == "slice" || expr.id == "toFixed") {
            std::string args;
            for (std::size_t i = 0; i < expr.operands.size(); ++i) {
                if (!expr.operands[i]) {
                    return fail("`" + expr.id + "` has a missing argument");
                }
                ExprEmission arg = emit_expr(*expr.operands[i], component, locals);
                if (!arg.error.empty()) {
                    return arg;
                }
                args += (i == 0 ? "" : ", ") + arg.text;
            }
            return ExprEmission{"tessera::ui::string_" + expr.id + "(" + args + ")", {}};
        }
        if (expr.id == "parseIntOr" || expr.id == "parseFloatOr") {
            // Issue #803: the strict whole-string parse. The fallback's C++ type decides which
            // instantiation is called, which is also why the IR validator requires the fallback
            // to be the matching type — an `int` fallback with a `float` parse would silently
            // truncate here.
            if (expr.operands.size() != 2 || !expr.operands[0] || !expr.operands[1]) {
                return fail("`" + expr.id + "` takes a string and a fallback");
            }
            ExprEmission text = emit_expr(*expr.operands[0], component, locals);
            if (!text.error.empty()) {
                return text;
            }
            ExprEmission fallback = emit_expr(*expr.operands[1], component, locals);
            if (!fallback.error.empty()) {
                return fallback;
            }
            return ExprEmission{
                "tessera::ui::parse_number_or(" + text.text + ", " + fallback.text + ")", {}};
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

bool reads_prop(const ir::Expr& expr) {
    if (expr.kind == ir::ExprKind::Ref && expr.id.starts_with("prop::")) {
        return true;
    }
    for (const ir::ExprPtr& operand : expr.operands) {
        if (operand && reads_prop(*operand)) {
            return true;
        }
    }
    for (const ir::TemplatePart& part : expr.parts) {
        if (part.expr && reads_prop(*part.expr)) {
            return true;
        }
    }
    for (const ir::RecordField& field : expr.fields) {
        if (field.value && reads_prop(*field.value)) {
            return true;
        }
    }
    return false;
}

std::string state_seeded_name(std::string_view state_id) {
    return member_name(state_id) + "seeded_";
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
