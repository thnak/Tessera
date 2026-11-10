// SPDX-License-Identifier: MIT

// Text interpolation collection and the generated apply_state()/update() path
// (docs/design/compiler.md section 11). The value typing, formatting helpers and hash/append
// emission live in value_ops.cpp and are shared with the keyed-list item content pass.

#include "emit_controlled.hpp"
#include "emit_dynamic.hpp"
#include "interpolation.hpp"
#include "value_ops.hpp"

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

// The interpolation source of a node: its `Text` payload, or a controlled field's value prop.
const ir::Expr* paint_expression(const ir::Node& node) {
    if (node.text != nullptr) {
        return node.text.get();
    }
    if (node.kind != ir::NodeKind::TextInput && node.kind != ir::NodeKind::TextArea) {
        return nullptr;
    }
    const auto value = node.props.find("value");
    if (value != node.props.end()) {
        return value->second.get();
    }
    const auto default_value = node.props.find("defaultValue");
    return default_value == node.props.end() ? nullptr : default_value->second.get();
}

bool collect_text(const ir::Node& node, const ir::Component& component, bool in_template,
                  unsigned& ordinal, std::vector<DynamicText>& entries, std::string& error) {
    const unsigned self = ++ordinal;
    if (node.loop) {
        in_template = true; // the whole template subtree is per item (handled by its list)
    }
    if (!in_template) {
        const ir::Expr* text_expr = paint_expression(node);
        if (text_expr != nullptr && !static_text_value(text_expr)) {
            if (!printable_text_expression(*text_expr, component)) {
                return fail(error, "text of `" + node.id + "` has no printable C++ type");
            }
            entries.push_back(DynamicText{self, text_expr});
        }
    }
    for (const ir::Node& child : node.children) {
        if (!collect_text(child, component, in_template, ordinal, entries, error)) {
            return false;
        }
    }
    // The item template of a virtual collection is not a child (ir-v1 section 3.5) but takes its
    // pre-order ordinals; its text is painted by the per-item content pass, not by apply_state().
    if (node.item_template &&
        !collect_text(*node.item_template, component, true, ordinal, entries, error)) {
        return false;
    }
    return true;
}

bool collect_metadata(const ir::Node& node, const ir::Component& component, bool in_template,
                      unsigned& ordinal, std::vector<DynamicMetadata>& entries,
                      std::string& error) {
    const unsigned self = ++ordinal;
    if (node.loop) {
        in_template = true;
    }
    if (!in_template) {
        const auto dynamic_value = [&](MetadataField field, const std::string& name,
                                       const ir::Expr* value) -> bool {
            if (value == nullptr) {
                return true;
            }
            if (static_text_value(value)) {
                return true; // a literal string stays inline in build()
            }
            if (!printable_text_expression(*value, component)) {
                return fail(error, "dynamic `" + name + "` on `" + node.id +
                                       "` has no printable C++ type");
            }
            entries.push_back(DynamicMetadata{self, field, name, value});
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
        for (const auto& [name, value] : node.style) {
            if (!dynamic_value(MetadataField::Style, name, value.get())) {
                return false;
            }
        }
    }
    for (const ir::Node& child : node.children) {
        if (!collect_metadata(child, component, in_template, ordinal, entries, error)) {
            return false;
        }
    }
    // Item templates keep their per-item metadata out of the component-level apply_state() table
    // (the plan rejects dynamic per-item metadata); the recursion keeps ordinals aligned.
    if (node.item_template &&
        !collect_metadata(*node.item_template, component, true, ordinal, entries, error)) {
        return false;
    }
    return true;
}

} // namespace

bool collect_dynamic_text(const ir::Node& node, const ir::Component& component, unsigned& ordinal,
                          std::vector<DynamicText>& entries, std::string& error) {
    return collect_text(node, component, false, ordinal, entries, error);
}

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
    case MetadataField::Style: {
        std::string name = "style_";
        for (const char c : entry.name) {
            name += (c == '-' || c == ':') ? '_' : c;
        }
        return base + name;
    }
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
    return collect_metadata(node, component, false, ordinal, entries, error);
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
                      const std::vector<DynamicMetadata>& metadata_entries,
                      const std::vector<ControlledInput>& controlled_inputs,
                      const DynamicPlan& plan, std::string& error) {
    writer.open("void " + component.name + "::apply_state()");
    // The child set first: a newly attached branch or list item owns nodes whose text the
    // interpolation below may then refresh in the same pass.
    for (const DynamicGroup& group : plan.groups) {
        writer.line("apply_children_" + std::to_string(group.parent_ordinal) + "();");
    }
    for (const DynamicText& entry : text_entries) {
        const std::string suffix = std::to_string(entry.ordinal);
        const std::string hash = "hash_" + suffix;
        writer.line("std::uint64_t " + hash + " = tessera::ui::kEffectHashSeed;");
        if (!emit_value_ops(writer, *entry.text, component, {}, hash, "", error)) {
            return false;
        }
        writer.open("if (!text_node_" + suffix + "_set_ || " + hash + " != text_node_" + suffix +
                    "_hash_)");
        ++writer.indent;
        writer.line("text_node_" + suffix + "_hash_ = " + hash + ";");
        writer.line("text_node_" + suffix + "_set_ = true;");
        writer.line("std::string next_" + suffix + ";");
        if (!emit_value_ops(writer, *entry.text, component, {}, "", "next_" + suffix, error)) {
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
        if (!emit_value_ops(writer, *entry.value, component, {}, hash, "", error)) {
            return false;
        }
        writer.open("if (!" + base + "_set_ || " + hash + " != " + base + "_hash_)");
        ++writer.indent;
        writer.line(base + "_hash_ = " + hash + ";");
        writer.line(base + "_set_ = true;");
        writer.line("std::string next_" + base + ";");
        if (!emit_value_ops(writer, *entry.value, component, {}, "", "next_" + base, error)) {
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
        case MetadataField::Style:
            writer.line(base + "_node_->set_inline_style(" +
                        literal_text(ir::LiteralValue{entry.name}) + ", " + base + "_value_);");
            // The node borrows the generated string, so an in-place SSO reassignment makes the
            // setter see equal views and skip its own Style mark; mark the domain explicitly.
            writer.line(base + "_node_->mark_dirty(tessera::ui::Dirty::Style);");
            break;
        }
        --writer.indent;
        writer.close("");
        --writer.indent;
        writer.line("}");
    }
    emit_controlled_apply(writer, controlled_inputs);
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
