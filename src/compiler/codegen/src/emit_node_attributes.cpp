// SPDX-License-Identifier: MIT

// What one scene node *is* (docs/design/compiler.md section 11): the rejection of the node kinds
// codegen cannot emit, the `std::make_unique<Node>` creation, and the element kind, id, className,
// automationId, role, `part`, aria-* and inline-style attributes. The behavioural groups (events,
// refs, focus, scroll, paint) live in emit_node_behavior.cpp; the order both files run in is the
// call list in emit_node.cpp.

#include "emit_node_groups.hpp"
#include "emit_node_support.hpp"
#include "emit_vector.hpp"
#include "emit_virtual.hpp"
#include "interpolation.hpp"
#include "value_ops.hpp"

#include <algorithm>
#include <cstdint>
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

} // namespace

const ir::Expr* NodeEmitContext::prop(std::string_view name) const {
    const auto found = node.props.find(std::string(name));
    return found == node.props.end() ? nullptr : found->second.get();
}

bool NodeEmitContext::is_dynamic_metadata(MetadataField field, std::string_view name) const {
    return std::any_of(state.dynamic_metadata.begin(), state.dynamic_metadata.end(),
                       [&](const DynamicMetadata& entry) {
                           return entry.ordinal == ordinal && entry.field == field &&
                                  entry.name == name;
                       });
}

NodeEmitContext::Outcome emit_node_prelude(NodeEmitContext& context) {
    const ir::Node& node = context.node;
    NodeEmitState& state = context.state;
    switch (node.kind) {
    case ir::NodeKind::CustomElement:
        fail(context.error, "`" + std::string(ir::node_kind_name(node.kind)) +
                                "` nodes are deferred (custom elements are a later codegen slice)");
        return NodeEmitContext::Outcome::Failed;
    case ir::NodeKind::VirtualList:
    case ir::NodeKind::VirtualGrid:
        // The collection is a plain fixed container; its item window is attached by
        // apply_virtual_<n>() and its item template is never built here.
        if (state.locals != nullptr) {
            fail(context.error, "a virtual collection (`" + node.id +
                                    "`) inside a list template is deferred; wrap it in an element");
            return NodeEmitContext::Outcome::Failed;
        }
        break;
    case ir::NodeKind::Component:
        // A component instance owns its own state and is never inlined; its host node and the
        // caller's slot content are emitted by the composition path. Inside a list template the
        // instance is one record of the item (docs/design/compiler-node-state.md section 3).
        return emit_component_node(context.writer, node, state, context.variable, context.error)
                   ? NodeEmitContext::Outcome::Emitted
                   : NodeEmitContext::Outcome::Failed;
    case ir::NodeKind::Slot:
        fail(context.error,
             "a Slot (`" + node.id + "`) is a child position, not a node the build() tree creates");
        return NodeEmitContext::Outcome::Failed;
    default:
        break;
    }
    if (state.plan == nullptr && (node.condition || node.loop)) {
        fail(context.error, "a dynamic child (`" + node.id +
                                "`) requires the dynamic plan; this is an internal codegen error");
        return NodeEmitContext::Outcome::Failed;
    }
    // Inside a list-item builder the template root keeps its `loop`; outside one, a loop node is a
    // template that build() never emits (the plan's group walk handles it).
    const bool item_builder = state.text_sites != nullptr;
    if (node.loop && !item_builder) {
        fail(context.error, "a loop template (`" + node.id +
                                "`) must be a child of a container; the component root cannot be a "
                                "template");
        return NodeEmitContext::Outcome::Failed;
    }
    // Every expression recorded in this node must have a C++ spelling even though the scene API
    // cannot apply it yet: an unsupported expression is rejected here, never silently dropped.
    for (const auto& [name, value] : node.props) {
        // Element metadata is emitted by the static/dynamic metadata paths below, which support
        // text interpolation; the generic expression validation only covers other props.
        if (name == "id" || name == "className" || name == "role" || name.rfind("aria-", 0) == 0) {
            continue;
        }
        // The `Path` vector payload owns these props (emit_vector.cpp) and rejects a non-literal
        // value with a diagnostic that names the prop; validating them here would report the
        // generic expression error first.
        if (node.kind == ir::NodeKind::Path &&
            (name == "d" || name == "viewBox" || name == "fill" || name == "stroke" ||
             name == "strokeWidth" || name == "label")) {
            continue;
        }
        if (value) {
            const ExprEmission emission = emit_expr(*value, state.component);
            if (!emission.error.empty()) {
                fail(context.error, "prop `" + name + "`: " + emission.error);
                return NodeEmitContext::Outcome::Failed;
            }
        }
    }
    if (node.style.size() > 16) {
        fail(context.error, "node `" + node.id + "` has more than 16 inline style declarations");
        return NodeEmitContext::Outcome::Failed;
    }
    for (const auto& [name, value] : node.style) {
        if (value && !printable_text_expression(*value, state.component)) {
            fail(context.error, "style `" + name + "` has no printable C++ type");
            return NodeEmitContext::Outcome::Failed;
        }
    }
    if (node.key && !item_builder) {
        // A key inside a list template reads loop locals and is emitted by apply_list_<n>(); the
        // node builder never sees it.
        const ExprEmission emission = emit_expr(*node.key, state.component);
        if (!emission.error.empty()) {
            fail(context.error, "key: " + emission.error);
            return NodeEmitContext::Outcome::Failed;
        }
    }
    context.item_builder = item_builder;
    return NodeEmitContext::Outcome::Proceed;
}

bool emit_node_creation(NodeEmitContext& context) {
    context.id = node_id_text(context.node, context.state, context.error);
    if (!context.error.empty()) {
        return false;
    }
    context.writer.line(node_comment(context.node));
    context.ordinal = context.state.next_var;
    context.text_site = find_text_site(context.state.text_sites, context.ordinal);
    context.dynamic =
        context.text_site != nullptr ||
        std::any_of(context.state.dynamic_text.begin(), context.state.dynamic_text.end(),
                    [&](const DynamicText& entry) { return entry.ordinal == context.ordinal; });
    context.variable = "node_" + std::to_string(context.state.next_var++);
    context.writer.line("auto " + context.variable + " = std::make_unique<tessera::ui::Node>(" +
                        context.id + ");");
    return true;
}

bool emit_element_kind(NodeEmitContext& context) {
    context.writer.line(context.variable + "->set_element_kind(\"" +
                        std::string(ir::node_kind_name(context.node.kind)) + "\");");
    if (context.node.kind != ir::NodeKind::VirtualList &&
        context.node.kind != ir::NodeKind::VirtualGrid) {
        return true;
    }
    if (context.state.plan == nullptr) {
        return fail(context.error,
                    "the virtual collection `" + context.node.id +
                        "` requires the dynamic plan; this is an internal codegen error");
    }
    context.virtual_collection = context.state.plan->virtual_collection(context.ordinal);
    if (context.virtual_collection == nullptr) {
        return fail(context.error,
                    "the virtual collection `" + context.node.id + "` is missing from the plan");
    }
    // The item window is attached to this fixed container node by apply_virtual_<n>().
    context.writer.line("this->" + item_table_prefix(*context.virtual_collection) +
                        "_ = " + context.variable + ".get();");
    return true;
}

void emit_dynamic_metadata_pointers(NodeEmitContext& context) {
    for (const DynamicMetadata& entry : context.state.dynamic_metadata) {
        if (entry.ordinal == context.ordinal) {
            context.writer.line(metadata_member(entry) + "_node_ = " + context.variable +
                                ".get();");
        }
    }
}

bool emit_element_metadata(NodeEmitContext& context) {
    if (const ir::Expr* id_prop = context.prop("id")) {
        const std::optional<std::string_view> text = literal_string(id_prop);
        if (text) {
            context.writer.line(context.variable + "->set_element_id(" +
                                literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!context.is_dynamic_metadata(MetadataField::ElementId, "id")) {
            return fail(context.error, "id: a dynamic element id has no printable type");
        }
    }
    if (const ir::Expr* class_prop = context.prop("className")) {
        const std::optional<std::string_view> text = literal_string(class_prop);
        if (text) {
            context.writer.line(context.variable + "->set_class_name(" +
                                literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!context.is_dynamic_metadata(MetadataField::ClassName, "className")) {
            return fail(context.error, "className: a dynamic class has no printable type");
        }
    }
    if (!context.node.automation_id.empty()) {
        const std::optional<std::uint64_t> value = parse_automation_id(context.node.automation_id);
        if (!value) {
            return fail(context.error, "node `" + context.node.id +
                                           "` has a non-numeric automationId `" +
                                           context.node.automation_id + "`");
        }
        context.writer.line(context.variable + "->set_automation_id(" + std::to_string(*value) +
                            "ull);");
    }
    if (const ir::Expr* role_prop = context.prop("role")) {
        const std::optional<std::string_view> text = literal_string(role_prop);
        if (text) {
            context.writer.line(context.variable + "->set_role(" +
                                literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!context.is_dynamic_metadata(MetadataField::Role, "role")) {
            return fail(context.error, "role: a dynamic role has no printable type");
        }
    }
    // `part` is the component library's styling API (ADR-047 item 3): a static list of names the
    // host's `::part()` selectors can target. It is never dynamic, like `role`.
    if (const ir::Expr* part_prop = context.prop("part")) {
        const std::optional<std::string_view> text = literal_string(part_prop);
        if (!text) {
            return fail(context.error, "part: a part list is a static string literal");
        }
        context.writer.line(context.variable + "->set_part(" +
                            literal_text(ir::LiteralValue{std::string(*text)}) + ");");
    }
    return true;
}

bool emit_aria_attributes(NodeEmitContext& context) {
    for (const auto& [name, value] : context.node.props) {
        if (name.rfind("aria-", 0) != 0) {
            continue;
        }
        const std::optional<std::string_view> text = literal_string(value.get());
        if (text) {
            context.writer.line(context.variable + "->set_aria(" +
                                literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                                literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!context.is_dynamic_metadata(MetadataField::Aria, name)) {
            return fail(context.error, name + ": a dynamic aria value has no printable type");
        }
    }
    if (context.node.kind == ir::NodeKind::Path) {
        // `Path.label` is the accessible name; `aria-label` is the channel the accessibility tree
        // resolves it from (accessibility-v1 section 2 name precedence).
        if (!emit_vector_label(context.writer, context.node, context.variable, context.error)) {
            return false;
        }
    }
    return true;
}

void emit_inline_style(NodeEmitContext& context) {
    for (const auto& [name, value] : context.node.style) {
        if (!value) {
            continue;
        }
        if (!static_text_value(value.get()) || value->kind == ir::ExprKind::Literal) {
            std::string_view text;
            if (const auto* literal = std::get_if<std::string>(&value->literal)) {
                text = *literal;
            }
            if (!text.empty()) {
                context.writer.line(context.variable + "->set_inline_style(" +
                                    literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                                    literal_text(ir::LiteralValue{std::string(text)}) + ");");
                continue;
            }
            if (const auto* integer = std::get_if<std::int64_t>(&value->literal)) {
                context.writer.line(context.variable + "->set_inline_style(" +
                                    literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                                    literal_text(ir::LiteralValue{std::to_string(*integer)}) +
                                    ");");
                continue;
            }
            if (std::holds_alternative<double>(value->literal)) {
                context.writer.line(context.variable + "->set_inline_style(" +
                                    literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                                    literal_text(value->literal) + ");");
                continue;
            }
        }
        // A non-literal style value is applied by apply_state() through its dynamic entry.
    }
}

} // namespace tessera::compiler::codegen::internal
