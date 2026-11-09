// SPDX-License-Identifier: MIT

// One node and its subtree -> native C++ (docs/design/compiler.md section 11). The per-node
// emission order is part of the contract: element kind, dynamic metadata node pointers, id,
// className, automationId, role, aria-*, style, events, controlled wiring, refs, focusability,
// scroll flags, paint text, then the child subtrees.
//
// A conditional branch is a child with `condition`: it is built into its generated ownership slot
// and attached by apply_state(). A list template (a child with `loop`) is not built here; its nodes
// are produced per item by the generated build_item_<n>().

#include "emit_node.hpp"
#include "interpolation.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
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

// `static_cast<tessera::ui::NodeId>(N)` for the static tree; inside a list-item builder the
// template's pre-order id is offset by the item index so every generated node instance has a unique
// NodeId (damage tracking keys previous bounds on it).
std::string node_id_text(const ir::Node& node, NodeEmitState& state, std::string& error) {
    std::string number;
    std::string suffix = "u";
    if (node.ui_id.empty()) {
        // Slice 10 computes uiId; until then the pre-order index is the deterministic placeholder.
        number = std::to_string(state.next_id++);
    } else {
        for (const char c : node.ui_id) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                error = "TESSERA_CODEGEN_UNSUPPORTED: node `" + node.id +
                        "` has a non-numeric uiId `" + node.ui_id + "`";
                return {};
            }
        }
        ++state.next_id;
        number = node.ui_id;
        suffix = "ull";
    }
    if (state.index_expr.empty()) {
        return "static_cast<tessera::ui::NodeId>(" + number + suffix + ")";
    }
    return "static_cast<tessera::ui::NodeId>(" + number + suffix + " + " + state.index_expr +
           " * kItemIdStride)";
}

// Metadata and event targets that still have no runtime API are recorded in a deterministic
// comment (docs/design/compiler.md section 11).
std::string node_comment(const ir::Node& node) {
    std::string out = "// " + node.id;
    if (!node.events.empty()) {
        out += " events={";
        bool first = true;
        for (const auto& [name, target] : node.events) {
            out += (first ? "" : ",") + name + ":" + target;
            first = false;
        }
        out += "}";
    }
    return out;
}

std::optional<std::string_view> literal_string(const ir::Expr* expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&expr->literal)) {
        return std::string_view(*text);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_automation_id(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// The item-builder text site for `ordinal`, or nullptr when the node is not an interpolated
// template text node.
const ListTextSite* find_text_site(const std::vector<ListTextSite>* sites, unsigned ordinal) {
    if (sites == nullptr) {
        return nullptr;
    }
    for (const ListTextSite& site : *sites) {
        if (site.ordinal == ordinal) {
            return &site;
        }
    }
    return nullptr;
}

} // namespace

bool emit_node(CppWriter& writer, const ir::Node& node, NodeEmitState& state, std::string& variable,
               std::string& error) {
    switch (node.kind) {
    case ir::NodeKind::CustomElement:
    case ir::NodeKind::Component:
    case ir::NodeKind::VirtualList:
    case ir::NodeKind::VirtualGrid:
        return fail(error, "`" + std::string(ir::node_kind_name(node.kind)) +
                               "` nodes are deferred (component composition, custom elements and "
                               "virtualized lists are later codegen slices)");
    default:
        break;
    }
    if (state.plan == nullptr && (node.condition || node.loop)) {
        return fail(error, "a dynamic child (`" + node.id +
                               "`) requires the dynamic plan; this is an internal codegen error");
    }
    // Inside a list-item builder the template root keeps its `loop`; outside one, a loop node is a
    // template that build() never emits (the plan's group walk handles it).
    const bool item_builder = state.text_sites != nullptr;
    if (node.loop && !item_builder) {
        return fail(error, "a loop template (`" + node.id +
                               "`) must be a child of a container; the component root cannot be a "
                               "template");
    }
    // Every expression recorded in this node must have a C++ spelling even though the scene API
    // cannot apply it yet: an unsupported expression is rejected here, never silently dropped.
    for (const auto& [name, value] : node.props) {
        // Element metadata is emitted by the static/dynamic metadata paths below, which support
        // text interpolation; the generic expression validation only covers other props.
        if (name == "id" || name == "className" || name == "role" || name.rfind("aria-", 0) == 0) {
            continue;
        }
        if (value) {
            const ExprEmission emission = emit_expr(*value, state.component);
            if (!emission.error.empty()) {
                return fail(error, "prop `" + name + "`: " + emission.error);
            }
        }
    }
    if (node.style.size() > 16) {
        return fail(error, "node `" + node.id + "` has more than 16 inline style declarations");
    }
    for (const auto& [name, value] : node.style) {
        if (value && !printable_text_expression(*value, state.component)) {
            return fail(error, "style `" + name + "` has no printable C++ type");
        }
    }
    if (node.key && !item_builder) {
        // A key inside a list template reads loop locals and is emitted by apply_list_<n>(); the
        // node builder never sees it.
        const ExprEmission emission = emit_expr(*node.key, state.component);
        if (!emission.error.empty()) {
            return fail(error, "key: " + emission.error);
        }
    }

    const std::string id = node_id_text(node, state, error);
    if (!error.empty()) {
        return false;
    }
    writer.line(node_comment(node));
    const unsigned node_ordinal = state.next_var;
    const ListTextSite* text_site = find_text_site(state.text_sites, node_ordinal);
    const bool dynamic =
        text_site != nullptr ||
        std::any_of(state.dynamic_text.begin(), state.dynamic_text.end(),
                    [&](const DynamicText& entry) { return entry.ordinal == node_ordinal; });
    variable = "node_" + std::to_string(state.next_var++);
    writer.line("auto " + variable + " = std::make_unique<tessera::ui::Node>(" + id + ");");
    const auto prop = [&](std::string_view name) -> const ir::Expr* {
        const auto found = node.props.find(std::string(name));
        return found == node.props.end() ? nullptr : found->second.get();
    };
    const auto is_dynamic_metadata = [&](MetadataField field, std::string_view name) {
        return std::any_of(state.dynamic_metadata.begin(), state.dynamic_metadata.end(),
                           [&](const DynamicMetadata& entry) {
                               return entry.ordinal == node_ordinal && entry.field == field &&
                                      entry.name == name;
                           });
    };
    writer.line(variable + "->set_element_kind(\"" + std::string(ir::node_kind_name(node.kind)) +
                "\");");
    for (const DynamicMetadata& entry : state.dynamic_metadata) {
        if (entry.ordinal == node_ordinal) {
            writer.line(metadata_member(entry) + "_node_ = " + variable + ".get();");
        }
    }
    if (const ir::Expr* id_prop = prop("id")) {
        const std::optional<std::string_view> text = literal_string(id_prop);
        if (text) {
            writer.line(variable + "->set_element_id(" +
                        literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!is_dynamic_metadata(MetadataField::ElementId, "id")) {
            return fail(error, "id: a dynamic element id has no printable type");
        }
    }
    if (const ir::Expr* class_prop = prop("className")) {
        const std::optional<std::string_view> text = literal_string(class_prop);
        if (text) {
            writer.line(variable + "->set_class_name(" +
                        literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!is_dynamic_metadata(MetadataField::ClassName, "className")) {
            return fail(error, "className: a dynamic class has no printable type");
        }
    }
    if (!node.automation_id.empty()) {
        const std::optional<std::uint64_t> value = parse_automation_id(node.automation_id);
        if (!value) {
            return fail(error, "node `" + node.id + "` has a non-numeric automationId `" +
                                   node.automation_id + "`");
        }
        writer.line(variable + "->set_automation_id(" + std::to_string(*value) + "ull);");
    }
    if (const ir::Expr* role_prop = prop("role")) {
        const std::optional<std::string_view> text = literal_string(role_prop);
        if (text) {
            writer.line(variable + "->set_role(" +
                        literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!is_dynamic_metadata(MetadataField::Role, "role")) {
            return fail(error, "role: a dynamic role has no printable type");
        }
    }
    for (const auto& [name, value] : node.props) {
        if (name.rfind("aria-", 0) != 0) {
            continue;
        }
        const std::optional<std::string_view> text = literal_string(value.get());
        if (text) {
            writer.line(variable + "->set_aria(" +
                        literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                        literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!is_dynamic_metadata(MetadataField::Aria, name)) {
            return fail(error, name + ": a dynamic aria value has no printable type");
        }
    }
    const ControlledInput* controlled_input = nullptr;
    for (const ControlledInput& input : state.controlled_inputs) {
        if (input.ordinal == node_ordinal) {
            controlled_input = &input;
            break;
        }
    }
    for (const auto& [name, value] : node.style) {
        if (!value) {
            continue;
        }
        if (!static_text_value(value.get()) || value->kind == ir::ExprKind::Literal) {
            std::string_view text;
            if (const auto* literal = std::get_if<std::string>(&value->literal)) {
                text = *literal;
            }
            if (!text.empty()) {
                writer.line(variable + "->set_inline_style(" +
                            literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                            literal_text(ir::LiteralValue{std::string(text)}) + ");");
                continue;
            }
            if (const auto* integer = std::get_if<std::int64_t>(&value->literal)) {
                writer.line(variable + "->set_inline_style(" +
                            literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                            literal_text(ir::LiteralValue{std::to_string(*integer)}) + ");");
                continue;
            }
            if (std::holds_alternative<double>(value->literal)) {
                writer.line(variable + "->set_inline_style(" +
                            literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                            literal_text(value->literal) + ");");
                continue;
            }
        }
        // A non-literal style value is applied by apply_state() through its dynamic entry.
    }
    for (const auto& [jsx_name, target] : node.events) {
        (void)target;
        if (controlled_input != nullptr && jsx_name == "onChange") {
            // Wired through TextChangeBinding below, not the generic scene-event adapter.
            continue;
        }
        const EventAdapter* adapter = nullptr;
        for (const EventAdapter& candidate : state.adapters) {
            if (candidate.ordinal == node_ordinal && candidate.jsx_name == jsx_name) {
                adapter = &candidate;
                break;
            }
        }
        if (adapter == nullptr) {
            return fail(error, "event `" + jsx_name + "` has no adapter");
        }
        writer.line(variable + "->bind_event(" + adapter->event_type + ", &" + adapter->function +
                    ", this);");
    }
    if (controlled_input != nullptr) {
        emit_controlled_wiring(writer, variable, *controlled_input);
    }
    for (const ir::Ref& ref : state.component.refs) {
        if (ref.target == node.id) {
            writer.line("this->" + member_name(ref.id) + " = " + variable + ".get();");
        }
    }
    if (node.kind == ir::NodeKind::Button || node.kind == ir::NodeKind::TextInput ||
        node.kind == ir::NodeKind::TextArea) {
        writer.line(variable + "->set_focusable(true);");
    }
    if (node.kind == ir::NodeKind::Scroll) {
        std::string axis = "y";
        const auto found = node.props.find("axis");
        if (found != node.props.end() && found->second &&
            found->second->kind == ir::ExprKind::Literal) {
            if (const auto* text = std::get_if<std::string>(&found->second->literal)) {
                axis = *text;
            }
        }
        writer.line(variable + "->set_scrollable_x(" +
                    (axis == "x" || axis == "both" ? "true" : "false") + ");");
        writer.line(variable + "->set_scrollable_y(" +
                    (axis == "y" || axis == "both" ? "true" : "false") + ");");
    }
    const ir::Expr* paint_expr = node.text.get();
    if (paint_expr == nullptr && controlled_input != nullptr) {
        paint_expr = controlled_input->value;
    }
    if (const std::optional<std::string> paint_text = static_text_value(paint_expr)) {
        // D9 paint payload: text is visual only (layout/wrap stay paint-side).
        const std::string paint = "paint_" + variable;
        writer.line("tessera::ui::PaintData " + paint + ";");
        writer.line(paint + ".text = " + literal_text(ir::LiteralValue{std::string(*paint_text)}) +
                    ";");
        writer.line(variable + "->set_paint_data(std::move(" + paint + "));");
        if (controlled_input != nullptr) {
            writer.line("this->" + controlled_input->editor_member + ".set_text(" +
                        literal_text(ir::LiteralValue{std::string(*paint_text)}) + ");");
        }
    } else if (text_site != nullptr) {
        // The per-item content pass paints this node from the loop item/index values.
        writer.line(state.text_out + "[" + std::to_string(text_site->site) + "] = " + variable +
                    ".get();");
    } else if (dynamic) {
        const std::string suffix = std::to_string(node_ordinal);
        writer.line("this->text_node_" + suffix + "_ = " + variable + ".get();");
    }
    if (state.plan != nullptr) {
        const DynamicGroup* group = state.plan->group(node_ordinal);
        if (group != nullptr) {
            writer.line("this->children_parent_" + std::to_string(node_ordinal) +
                        "_ = " + variable + ".get();");
        }
    }
    for (const ir::Node& child : node.children) {
        const unsigned child_ordinal = state.next_id;
        const DynamicChildKind kind = state.plan != nullptr
                                          ? state.plan->child_kind(node_ordinal, child_ordinal)
                                          : DynamicChildKind::Static;
        if (kind == DynamicChildKind::List) {
            const ListTemplate* list = state.plan->list(child_ordinal);
            writer.line("// " + child.id +
                        " is a list template: its items are built by build_item_" +
                        std::to_string(child_ordinal) + "() (bounded item table)");
            if (list == nullptr) {
                return fail(error, "the list template `" + child.id + "` is missing from the plan");
            }
            state.next_id = list->subtree_end;
            state.next_var = list->subtree_end;
            continue;
        }
        std::string child_variable;
        if (!emit_node(writer, child, state, child_variable, error)) {
            return false;
        }
        if (kind == DynamicChildKind::Conditional) {
            writer.line("this->" + branch_member(child_ordinal, "owned_") + " = std::move(" +
                        child_variable + ");");
        } else {
            writer.line(variable + "->add_child(std::move(" + child_variable + "));");
        }
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
