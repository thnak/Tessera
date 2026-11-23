// SPDX-License-Identifier: MIT

// Text value typing and the hash/append emission shared by apply_state() and the generated
// keyed-list item content pass (docs/design/compiler.md section 11).

#include "value_ops.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::compiler::codegen::internal {

// Defined below (and used by `dotted_text_type` and the loop-item path): the declared type of a
// record member.
std::string_view record_member_type(std::span<const ir::RecordType> records,
                                    std::string_view type_name, std::string_view name);

namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::optional<TextValueType> local_text_type(const LocalBindings& locals, std::string_view id,
                                             std::span<const ir::RecordType> records) {
    if (const LocalBinding* local = find_local(locals, id)) {
        return text_value_type(local->ir_type);
    }
    // A record loop item read as `row.name` (issue #834): the field's declared type through the
    // module's record table, so a list of records can render `{row.name}`.
    const std::size_t dot = id.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const LocalBinding* local = find_local(locals, id.substr(0, dot));
    if (local == nullptr) {
        return std::nullopt;
    }
    std::string_view type = local->ir_type;
    std::string_view rest = id.substr(dot + 1);
    while (!rest.empty()) {
        const std::size_t next = rest.find('.');
        const std::string_view name = next == std::string_view::npos ? rest : rest.substr(0, next);
        type = record_member_type(records, type, name);
        if (type.empty()) {
            return std::nullopt;
        }
        if (next == std::string_view::npos) {
            break;
        }
        rest = rest.substr(next + 1);
    }
    return text_value_type(type);
}

std::optional<TextValueType> infer_type(const ir::Expr& expr, const ir::Component& component,
                                        std::span<const ir::RecordType> records,
                                        const LocalBindings& locals) {
    switch (expr.kind) {
    case ir::ExprKind::Literal:
        if (std::holds_alternative<bool>(expr.literal)) {
            return TextValueType::Bool;
        }
        if (std::holds_alternative<std::int64_t>(expr.literal)) {
            return TextValueType::Int;
        }
        if (std::holds_alternative<double>(expr.literal)) {
            return TextValueType::Float;
        }
        if (std::holds_alternative<std::string>(expr.literal)) {
            return TextValueType::String;
        }
        return std::nullopt;
    case ir::ExprKind::Ref:
        if (const std::optional<TextValueType> local = local_text_type(locals, expr.id, records)) {
            return local;
        }
        return declared_text_type(component, records, expr.id);
    case ir::ExprKind::Template:
        return TextValueType::String;
    case ir::ExprKind::Binary: {
        static constexpr std::string_view kBoolOps[] = {
            "==", "!=", "<", "<=", ">", ">=", "&&", "||"};
        for (const std::string_view op : kBoolOps) {
            if (expr.op == op) {
                return TextValueType::Bool;
            }
        }
        if (expr.operands.size() != 2 || !expr.operands[0] || !expr.operands[1]) {
            return std::nullopt;
        }
        const std::optional<TextValueType> left =
            infer_type(*expr.operands[0], component, records, locals);
        const std::optional<TextValueType> right =
            infer_type(*expr.operands[1], component, records, locals);
        if (!left || !right || *left == TextValueType::String || *right == TextValueType::String ||
            *left == TextValueType::Bool || *right == TextValueType::Bool) {
            return std::nullopt;
        }
        if (expr.op == "/" || *left == TextValueType::Float || *right == TextValueType::Float) {
            return TextValueType::Float;
        }
        return TextValueType::Int;
    }
    case ir::ExprKind::Unary:
        if (expr.op == "!") {
            return TextValueType::Bool;
        }
        if (expr.operands.size() != 1 || !expr.operands[0]) {
            return std::nullopt;
        }
        return infer_type(*expr.operands[0], component, records, locals);
    case ir::ExprKind::Conditional: {
        if (expr.operands.size() != 3 || !expr.operands[1] || !expr.operands[2]) {
            return std::nullopt;
        }
        const std::optional<TextValueType> then_type =
            infer_type(*expr.operands[1], component, records, locals);
        const std::optional<TextValueType> else_type =
            infer_type(*expr.operands[2], component, records, locals);
        if (!then_type || !else_type || *then_type != *else_type) {
            return std::nullopt;
        }
        return then_type;
    }
    case ir::ExprKind::Call:
        if (expr.id == "toInt") {
            return TextValueType::Int;
        }
        if (expr.id == "toFloat" || expr.id == "floor" || expr.id == "ceil" || expr.id == "round") {
            return TextValueType::Float;
        }
        if (expr.id == "length" || expr.id == "formatNumber") {
            return TextValueType::String;
        }
        if (expr.operands.empty() || !expr.operands[0]) {
            return std::nullopt;
        }
        return infer_type(*expr.operands[0], component, records, locals);
    case ir::ExprKind::Array:
        // A list literal has no printable text form (a list is not interpolated in v1).
        return std::nullopt;
    case ir::ExprKind::Record:
        // A record value has no printable text form either (section 4.3: a value position only).
        return std::nullopt;
    case ir::ExprKind::OptionalField:
    case ir::ExprKind::Coalesce:
        // ADR-066 item 1: `a?.b` and `a ?? b` are printable when their result is, which is the
        // fallback's type for a coalesce and the field's for an optional access. Neither is
        // inferable here without the record table, so they are not printable in a text position.
        return std::nullopt;
    case ir::ExprKind::ListFilter:
    case ir::ExprKind::ListSlice:
        // ADR-066 item 3: a list operation yields a list, which is not interpolated in v1.
        return std::nullopt;
    case ir::ExprKind::RecordUpdate:
        // A record update yields a record, which has no printable text form either.
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

std::optional<TextValueType> text_value_type(std::string_view ir_type) {
    if (ir_type == "bool") {
        return TextValueType::Bool;
    }
    if (ir_type == "int") {
        return TextValueType::Int;
    }
    if (ir_type == "float" || ir_type == "length") {
        return TextValueType::Float;
    }
    if (ir_type == "string" || ir_type == "color") {
        return TextValueType::String;
    }
    return std::nullopt;
}

// The declared IR type of a component field reference (`state::x`, `prop::x`, `anim::x`), or an
// empty view when `id` names no declared cell. A dotted id is *not* resolved here: the caller
// splits it first, because the base has to be looked up and the rest walked through the record
// table.
std::string_view declared_ir_type(const ir::Component& component, std::string_view id) {
    for (const ir::State& state : component.state) {
        if (state.id == id) {
            return state.type;
        }
    }
    for (const ir::Prop& prop : component.props) {
        if ("prop::" + prop.name == id) {
            return prop.type;
        }
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (value.id == id) {
            return value.type;
        }
    }
    // An `environment` hook value (issue #692): `useBreakpoint()` returns a string, so a Text that
    // renders it is printable like any other string cell.
    for (const ir::Environment& env : component.environment) {
        if (env.id == id) {
            return env.type;
        }
    }
    // A behavior output (ADR-064 item 7): `beh::<name>.<field>` is printable when the manifest
    // declares the field's type, which is what makes `<Text>{press.pressed}</Text>` work.
    for (const ir::Behavior& behavior : component.behaviors) {
        for (const ir::BehaviorOutput& output : behavior.outputs) {
            if (behavior.id + "." + output.name == id) {
                return output.type;
            }
        }
    }
    return {};
}

// One step of a member path: the declared type of field `name` in record `type_name`, or an empty
// view when either the record or the member is unknown.
std::string_view record_member_type(std::span<const ir::RecordType> records,
                                    std::string_view type_name, std::string_view name) {
    for (const ir::RecordType& record : records) {
        if (record.name != type_name) {
            continue;
        }
        for (const ir::RecordMember& member : record.members) {
            if (member.name == name) {
                return member.type;
            }
        }
        return {};
    }
    return {};
}

// The text family of a dotted id: the longest cell prefix that names a declared cell decides the
// base, and the remainder is a member path (ir-v1-expr.md section 4.3, "dotted members"). The
// evaluator resolves `ExprKind::Ref` exactly this way and is the reference implementation.
//
// A record-typed *cell* has no printable form; its *member* does. The lookup therefore has to reach
// the member's declared type, never the cell's, which is what the first loop's `text_value_type`
// would otherwise report.
std::optional<TextValueType> dotted_text_type(const ir::Component& component,
                                              std::span<const ir::RecordType> records,
                                              std::string_view id) {
    // Walk the dots from the right: the base is the longest prefix that names a cell, so `a.b.c`
    // tries `a.b` before `a` when both could be cells.
    for (std::size_t dot = id.rfind('.'); dot != std::string_view::npos;
         dot = dot == 0 ? std::string_view::npos : id.rfind('.', dot - 1)) {
        const std::string_view base = id.substr(0, dot);
        const std::string_view type_name = declared_ir_type(component, base);
        if (type_name.empty()) {
            continue;
        }
        // The base names a cell whose type is a record: walk the remaining path through the table.
        std::string_view current = type_name;
        std::string_view rest = id.substr(dot + 1);
        while (!rest.empty()) {
            const std::size_t next = rest.find('.');
            const std::string_view name =
                next == std::string_view::npos ? rest : rest.substr(0, next);
            current = record_member_type(records, current, name);
            if (current.empty()) {
                return std::nullopt;
            }
            if (next == std::string_view::npos) {
                break;
            }
            rest = rest.substr(next + 1);
        }
        return text_value_type(current);
    }
    return std::nullopt;
}

std::optional<TextValueType> declared_text_type(const ir::Component& component,
                                                std::span<const ir::RecordType> records,
                                                std::string_view id) {
    if (const std::string_view type = declared_ir_type(component, id); !type.empty()) {
        return text_value_type(type);
    }
    // A dotted member read into a record-typed cell (#827). Only reached when the whole id is not
    // itself a declared name, so an id that happens to contain a dot but names a cell still takes
    // the branch above.
    return dotted_text_type(component, records, id);
}

std::optional<TextValueType> infer_text_type(const ir::Expr& expr, const ir::Component& component,
                                             std::span<const ir::RecordType> records,
                                             const LocalBindings& locals) {
    return infer_type(expr, component, records, locals);
}

bool printable_text_expression(const ir::Expr& expr, const ir::Component& component,
                               std::span<const ir::RecordType> records,
                               const LocalBindings& locals) {
    return infer_text_type(expr, component, records, locals).has_value();
}

bool emit_value_ops(CppWriter& writer, const ir::Expr& expr, const ir::Component& component,
                    std::span<const ir::RecordType> records, const LocalBindings& locals,
                    const std::string& hash_var, const std::string& append_target,
                    std::string& error) {
    const auto hash_literal = [&](std::string_view value) {
        // A bare string literal would bind the hash overload for bool (pointer conversion); wrap
        // it so the string_view overload is selected.
        writer.line(hash_var + " = tessera::ui::effect_hash(" + hash_var + ", std::string_view(" +
                    std::string(value) + "));");
    };
    const auto append_line = [&](std::string_view value) {
        writer.line(append_target + " += " + std::string(value) + ";");
    };
    const auto emit_expression = [&](const ir::Expr& value) -> bool {
        const ExprEmission emission = emit_expr(value, component, locals);
        if (!emission.error.empty()) {
            return fail(error, emission.error);
        }
        if (!hash_var.empty()) {
            const std::optional<TextValueType> type = infer_type(value, component, records, locals);
            if (type == TextValueType::String) {
                writer.line(hash_var + " = tessera::ui::effect_hash(" + hash_var +
                            ", std::string_view(" + emission.text + "));");
            } else {
                writer.line(hash_var + " = tessera::ui::effect_hash(" + hash_var + ", " +
                            emission.text + ");");
            }
        } else {
            append_line("format_value(" + emission.text + ")");
        }
        return true;
    };

    if (expr.kind == ir::ExprKind::Template) {
        for (const ir::TemplatePart& part : expr.parts) {
            if (!part.expr) {
                const std::string literal = literal_text(ir::LiteralValue{part.text});
                if (!hash_var.empty()) {
                    hash_literal(literal);
                } else {
                    append_line(literal);
                }
                continue;
            }
            if (part.expr->kind == ir::ExprKind::Template ||
                part.expr->kind == ir::ExprKind::Conditional) {
                if (!emit_value_ops(writer, *part.expr, component, records, locals, hash_var,
                                    append_target, error)) {
                    return false;
                }
                continue;
            }
            if (!infer_type(*part.expr, component, records, locals)) {
                return fail(error, "a text interpolation has no printable C++ type");
            }
            if (!emit_expression(*part.expr)) {
                return false;
            }
        }
        return true;
    }
    if (expr.kind == ir::ExprKind::Conditional) {
        if (expr.operands.size() != 3 || !expr.operands[0] || !expr.operands[1] ||
            !expr.operands[2]) {
            return fail(error, "a conditional needs three operands");
        }
        const ExprEmission test = emit_expr(*expr.operands[0], component, locals);
        if (!test.error.empty()) {
            return fail(error, test.error);
        }
        if (!infer_type(*expr.operands[1], component, records, locals) ||
            !infer_type(*expr.operands[2], component, records, locals)) {
            return fail(error, "a text conditional has no printable C++ type");
        }
        std::string test_text = test.text;
        if (test_text.size() < 2 || test_text.front() != '(' || test_text.back() != ')') {
            test_text = "(" + test_text + ")";
        }
        writer.line("if " + test_text + " {");
        ++writer.indent;
        if (!emit_value_ops(writer, *expr.operands[1], component, records, locals, hash_var,
                            append_target, error)) {
            return false;
        }
        --writer.indent;
        writer.line("} else {");
        ++writer.indent;
        if (!emit_value_ops(writer, *expr.operands[2], component, records, locals, hash_var,
                            append_target, error)) {
            return false;
        }
        --writer.indent;
        writer.line("}");
        return true;
    }
    if (!infer_type(expr, component, records, locals)) {
        return fail(error, "a text interpolation has no printable C++ type");
    }
    return emit_expression(expr);
}

} // namespace tessera::compiler::codegen::internal
