// SPDX-License-Identifier: MIT

// Text interpolation and the generated apply_state()/update() path
// (docs/design/compiler.md section 11).

#include "emit_events.hpp"
#include "interpolation.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::optional<TextValueType> declared_text_type(const ir::Component& component,
                                                std::string_view id) {
    const auto map = [](std::string_view type) -> std::optional<TextValueType> {
        if (type == "bool") {
            return TextValueType::Bool;
        }
        if (type == "int") {
            return TextValueType::Int;
        }
        if (type == "float" || type == "length") {
            return TextValueType::Float;
        }
        if (type == "string" || type == "color") {
            return TextValueType::String;
        }
        return std::nullopt;
    };
    for (const ir::State& state : component.state) {
        if (state.id == id) {
            return map(state.type);
        }
    }
    for (const ir::Prop& prop : component.props) {
        if ("prop::" + prop.name == id) {
            return map(prop.type);
        }
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (value.id == id) {
            return map(value.type);
        }
    }
    return std::nullopt;
}

std::optional<TextValueType> infer_type(const ir::Expr& expr, const ir::Component& component) {
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
        const std::optional<TextValueType> left = infer_type(*expr.operands[0], component);
        const std::optional<TextValueType> right = infer_type(*expr.operands[1], component);
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
        return infer_type(*expr.operands[0], component);
    case ir::ExprKind::Conditional: {
        if (expr.operands.size() != 3 || !expr.operands[1] || !expr.operands[2]) {
            return std::nullopt;
        }
        const std::optional<TextValueType> then_type = infer_type(*expr.operands[1], component);
        const std::optional<TextValueType> else_type = infer_type(*expr.operands[2], component);
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
        return infer_type(*expr.operands[0], component);
    }
    return std::nullopt;
}

// Emits the hash/append statements for one value. `hash_call` writes `hash_var = effect_hash(...)`
// lines; `append_target` names the string that receives `+=` lines. Exactly one mode is active.
bool emit_value_ops(CppWriter& writer, const ir::Expr& expr, const ir::Component& component,
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
        const ExprEmission emission = emit_expr(value, component);
        if (!emission.error.empty()) {
            return fail(error, emission.error);
        }
        if (!hash_var.empty()) {
            const std::optional<TextValueType> type = infer_type(value, component);
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
                if (!emit_value_ops(writer, *part.expr, component, hash_var, append_target,
                                    error)) {
                    return false;
                }
                continue;
            }
            if (!infer_type(*part.expr, component)) {
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
        const ExprEmission test = emit_expr(*expr.operands[0], component);
        if (!test.error.empty()) {
            return fail(error, test.error);
        }
        if (!infer_type(*expr.operands[1], component) ||
            !infer_type(*expr.operands[2], component)) {
            return fail(error, "a text conditional has no printable C++ type");
        }
        std::string test_text = test.text;
        if (test_text.size() < 2 || test_text.front() != '(' || test_text.back() != ')') {
            test_text = "(" + test_text + ")";
        }
        writer.line("if " + test_text + " {");
        ++writer.indent;
        if (!emit_value_ops(writer, *expr.operands[1], component, hash_var, append_target, error)) {
            return false;
        }
        --writer.indent;
        writer.line("} else {");
        ++writer.indent;
        if (!emit_value_ops(writer, *expr.operands[2], component, hash_var, append_target, error)) {
            return false;
        }
        --writer.indent;
        writer.line("}");
        return true;
    }
    if (!infer_type(expr, component)) {
        return fail(error, "a text interpolation has no printable C++ type");
    }
    return emit_expression(expr);
}

} // namespace

std::optional<std::string> static_text_value(const ir::Expr* expr) {
    if (expr == nullptr) {
        return std::nullopt;
    }
    if (expr->kind == ir::ExprKind::Literal) {
        if (const auto* text = std::get_if<std::string>(&expr->literal)) {
            return *text;
        }
        return std::nullopt;
    }
    if (expr->kind != ir::ExprKind::Template) {
        return std::nullopt;
    }
    std::string folded;
    for (const ir::TemplatePart& part : expr->parts) {
        if (part.expr) {
            return std::nullopt;
        }
        folded += part.text;
    }
    return folded;
}

std::string metadata_member(const DynamicMetadata& entry) {
    std::string base = "meta_" + std::to_string(entry.ordinal) + "_";
    switch (entry.field) {
    case MetadataField::ElementId:
        return base + "id";
    case MetadataField::ClassName:
        return base + "class";
    case MetadataField::Role:
        return base + "role";
    case MetadataField::Aria: {
        std::string name = "aria_";
        for (const char c : entry.name) {
            name += (c == '-' || c == ':') ? '_' : c;
        }
        return base + name;
    }
    }
    return base;
}

bool collect_dynamic_metadata(const ir::Node& node, const ir::Component& component,
                              unsigned& ordinal, std::vector<DynamicMetadata>& entries,
                              std::string& error) {
    ++ordinal;
    const auto dynamic_value = [&](MetadataField field, const std::string& name,
                                   const ir::Expr* value) -> bool {
        if (value == nullptr) {
            return true;
        }
        if (static_text_value(value)) {
            return true; // literal string stays inline in build()
        }
        if (!infer_type(*value, component)) {
            return fail(error,
                        "dynamic `" + name + "` on `" + node.id + "` has no printable C++ type");
        }
        entries.push_back(DynamicMetadata{ordinal, field, name, value});
        return true;
    };
    const auto prop = [&](std::string_view name) -> const ir::Expr* {
        const auto found = node.props.find(std::string(name));
        return found == node.props.end() ? nullptr : found->second.get();
    };
    if (!dynamic_value(MetadataField::ElementId, "id", prop("id")) ||
        !dynamic_value(MetadataField::ClassName, "className", prop("className")) ||
        !dynamic_value(MetadataField::Role, "role", prop("role"))) {
        return false;
    }
    for (const auto& [name, value] : node.props) {
        if (name.rfind("aria-", 0) != 0) {
            continue;
        }
        if (!dynamic_value(MetadataField::Aria, name, value.get())) {
            return false;
        }
    }
    for (const ir::Node& child : node.children) {
        if (!collect_dynamic_metadata(child, component, ordinal, entries, error)) {
            return false;
        }
    }
    return true;
}

bool collect_dynamic_text(const ir::Node& node, const ir::Component& component, unsigned& ordinal,
                          std::vector<DynamicText>& entries, std::string& error) {
    ++ordinal;
    if (node.text && !static_text_value(node.text.get())) {
        if (!infer_type(*node.text, component)) {
            return fail(error, "text of `" + node.id + "` has no printable C++ type");
        }
        entries.push_back(DynamicText{ordinal, node.text.get()});
    }
    for (const ir::Node& child : node.children) {
        if (!collect_dynamic_text(child, component, ordinal, entries, error)) {
            return false;
        }
    }
    return true;
}

void emit_format_helpers(CppWriter& writer) {
    writer.open("namespace");
    writer.line("[[maybe_unused]] std::string format_value(bool value) {");
    ++writer.indent;
    writer.line("return value ? \"true\" : \"false\";");
    --writer.indent;
    writer.line("}");
    writer.line("[[maybe_unused]] std::string format_value(int value) {");
    ++writer.indent;
    writer.line("return std::to_string(value);");
    --writer.indent;
    writer.line("}");
    writer.line("[[maybe_unused]] std::string format_value(std::int64_t value) {");
    ++writer.indent;
    writer.line("return std::to_string(value);");
    --writer.indent;
    writer.line("}");
    writer.line("[[maybe_unused]] std::string format_value(double value) {");
    ++writer.indent;
    writer.line("char buffer[32];");
    writer.line("const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);");
    writer.line("return std::string(buffer, result.ptr);");
    --writer.indent;
    writer.line("}");
    writer.line("[[maybe_unused]] std::string format_value(std::string_view value) {");
    ++writer.indent;
    writer.line("return std::string(value);");
    --writer.indent;
    writer.line("}");
    writer.line("[[maybe_unused]] std::string format_value(const std::string& value) {");
    ++writer.indent;
    writer.line("return value;");
    --writer.indent;
    writer.line("}");
    writer.line("[[maybe_unused]] std::string format_value(const char* value) {");
    ++writer.indent;
    writer.line("return std::string(value);");
    --writer.indent;
    writer.line("}");
    writer.close("");
    writer.line("");
}

bool emit_apply_state(CppWriter& writer, const ir::Component& component,
                      const std::vector<DynamicText>& text_entries,
                      const std::vector<DynamicMetadata>& metadata_entries, std::string& error) {
    writer.open("void " + component.name + "::apply_state()");
    for (const DynamicText& entry : text_entries) {
        const std::string suffix = std::to_string(entry.ordinal);
        const std::string hash = "hash_" + suffix;
        writer.line("std::uint64_t " + hash + " = tessera::ui::kEffectHashSeed;");
        if (!emit_value_ops(writer, *entry.text, component, hash, "", error)) {
            return false;
        }
        writer.open("if (!text_node_" + suffix + "_set_ || " + hash + " != text_node_" + suffix +
                    "_hash_)");
        ++writer.indent;
        writer.line("text_node_" + suffix + "_hash_ = " + hash + ";");
        writer.line("text_node_" + suffix + "_set_ = true;");
        writer.line("std::string next_" + suffix + ";");
        if (!emit_value_ops(writer, *entry.text, component, "", "next_" + suffix, error)) {
            return false;
        }
        writer.line("text_node_" + suffix + "_value_ = std::move(next_" + suffix + ");");
        writer.line("tessera::ui::PaintData paint_" + suffix + ";");
        writer.line("paint_" + suffix + ".text = text_node_" + suffix + "_value_;");
        writer.line("text_node_" + suffix + "_->set_paint_data(std::move(paint_" + suffix + "));");
        --writer.indent;
        writer.close("");
    }
    for (const DynamicMetadata& entry : metadata_entries) {
        const std::string base = metadata_member(entry);
        const std::string hash = base + "_hash";
        writer.line("std::uint64_t " + hash + " = tessera::ui::kEffectHashSeed;");
        if (!emit_value_ops(writer, *entry.value, component, hash, "", error)) {
            return false;
        }
        writer.open("if (!" + base + "_set_ || " + hash + " != " + base + "_hash_)");
        ++writer.indent;
        writer.line(base + "_hash_ = " + hash + ";");
        writer.line(base + "_set_ = true;");
        writer.line("std::string next_" + base + ";");
        if (!emit_value_ops(writer, *entry.value, component, "", "next_" + base, error)) {
            return false;
        }
        writer.line("if (" + base + "_value_ != next_" + base + ") {");
        ++writer.indent;
        // Move the old buffer out before overwriting: the node borrows a view into `_value_`, so
        // assigning in place would mutate the view before the setter can compare it.
        writer.line("std::string previous_" + base + " = std::move(" + base + "_value_);");
        writer.line(base + "_value_ = std::move(next_" + base + ");");
        switch (entry.field) {
        case MetadataField::ElementId:
            writer.line(base + "_node_->set_element_id(" + base + "_value_);");
            // The node borrows a view into `_value_`: for SSO strings an in-place update can make
            // the setter see equal content and skip its Style mark, so mark the selector domain
            // explicitly (idempotent when the setter already did).
            writer.line(base + "_node_->mark_dirty(tessera::ui::Dirty::Style);");
            break;
        case MetadataField::ClassName:
            writer.line(base + "_node_->set_class_name(" + base + "_value_);");
            writer.line(base + "_node_->mark_dirty(tessera::ui::Dirty::Style);");
            break;
        case MetadataField::Role:
            writer.line(base + "_node_->set_role(" + base + "_value_);");
            break;
        case MetadataField::Aria:
            writer.line(base + "_node_->set_aria(" + literal_text(ir::LiteralValue{entry.name}) +
                        ", " + base + "_value_);");
            break;
        }
        --writer.indent;
        writer.close("");
        --writer.indent;
        writer.line("}");
    }
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
