// SPDX-License-Identifier: MIT

// Text value typing and the hash/append emission shared by apply_state() and the generated
// keyed-list item content pass (docs/design/compiler.md section 11).

#include "value_ops.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::optional<TextValueType> local_text_type(const LocalBindings& locals, std::string_view id) {
    const LocalBinding* local = find_local(locals, id);
    if (local == nullptr) {
        return std::nullopt;
    }
    return text_value_type(local->ir_type);
}

std::optional<TextValueType> infer_type(const ir::Expr& expr, const ir::Component& component,
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
        if (const std::optional<TextValueType> local = local_text_type(locals, expr.id)) {
            return local;
        }
        return declared_text_type(component, expr.id);
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
        const std::optional<TextValueType> left = infer_type(*expr.operands[0], component, locals);
        const std::optional<TextValueType> right = infer_type(*expr.operands[1], component, locals);
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
        return infer_type(*expr.operands[0], component, locals);
    case ir::ExprKind::Conditional: {
        if (expr.operands.size() != 3 || !expr.operands[1] || !expr.operands[2]) {
            return std::nullopt;
        }
        const std::optional<TextValueType> then_type =
            infer_type(*expr.operands[1], component, locals);
        const std::optional<TextValueType> else_type =
            infer_type(*expr.operands[2], component, locals);
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
        return infer_type(*expr.operands[0], component, locals);
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

std::optional<TextValueType> declared_text_type(const ir::Component& component,
                                                std::string_view id) {
    for (const ir::State& state : component.state) {
        if (state.id == id) {
            return text_value_type(state.type);
        }
    }
    for (const ir::Prop& prop : component.props) {
        if ("prop::" + prop.name == id) {
            return text_value_type(prop.type);
        }
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (value.id == id) {
            return text_value_type(value.type);
        }
    }
    return std::nullopt;
}

std::optional<TextValueType> infer_text_type(const ir::Expr& expr, const ir::Component& component,
                                             const LocalBindings& locals) {
    return infer_type(expr, component, locals);
}

bool printable_text_expression(const ir::Expr& expr, const ir::Component& component,
                               const LocalBindings& locals) {
    return infer_text_type(expr, component, locals).has_value();
}

bool emit_value_ops(CppWriter& writer, const ir::Expr& expr, const ir::Component& component,
                    const LocalBindings& locals, const std::string& hash_var,
                    const std::string& append_target, std::string& error) {
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
            const std::optional<TextValueType> type = infer_type(value, component, locals);
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
                if (!emit_value_ops(writer, *part.expr, component, locals, hash_var, append_target,
                                    error)) {
                    return false;
                }
                continue;
            }
            if (!infer_type(*part.expr, component, locals)) {
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
        if (!infer_type(*expr.operands[1], component, locals) ||
            !infer_type(*expr.operands[2], component, locals)) {
            return fail(error, "a text conditional has no printable C++ type");
        }
        std::string test_text = test.text;
        if (test_text.size() < 2 || test_text.front() != '(' || test_text.back() != ')') {
            test_text = "(" + test_text + ")";
        }
        writer.line("if " + test_text + " {");
        ++writer.indent;
        if (!emit_value_ops(writer, *expr.operands[1], component, locals, hash_var, append_target,
                            error)) {
            return false;
        }
        --writer.indent;
        writer.line("} else {");
        ++writer.indent;
        if (!emit_value_ops(writer, *expr.operands[2], component, locals, hash_var, append_target,
                            error)) {
            return false;
        }
        --writer.indent;
        writer.line("}");
        return true;
    }
    if (!infer_type(expr, component, locals)) {
        return fail(error, "a text interpolation has no printable C++ type");
    }
    return emit_expression(expr);
}

} // namespace tessera::compiler::codegen::internal
