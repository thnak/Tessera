// SPDX-License-Identifier: MIT

// One node and its subtree -> native C++ (docs/design/compiler.md section 11). The per-node
// emission order is part of the contract: element kind, dynamic metadata node pointers, id,
// className, automationId, role, aria-*, style, events, controlled wiring, refs, focusability,
// scroll flags, paint text, then the child subtrees.
//
// A conditional branch is a child with `condition`: it is built into its generated ownership slot
// and attached by apply_state(). A list template (a child with `loop`) is not built here; its nodes
// are produced per item by the generated build_item_<n>().

#include "emit_layout_observe.hpp"
#include "emit_node.hpp"
#include "emit_node_support.hpp"
#include "emit_vector.hpp"
#include "emit_virtual.hpp"
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

} // namespace

bool emit_node(CppWriter& writer, const ir::Node& node, NodeEmitState& state, std::string& variable,
               std::string& error) {
    switch (node.kind) {
    case ir::NodeKind::CustomElement:
        return fail(error, "`" + std::string(ir::node_kind_name(node.kind)) +
                               "` nodes are deferred (custom elements are a later codegen slice)");
    case ir::NodeKind::VirtualList:
    case ir::NodeKind::VirtualGrid:
        // The collection is a plain fixed container; its item window is attached by
        // apply_virtual_<n>() and its item template is never built here.
        if (state.locals != nullptr) {
            return fail(error, "a virtual collection (`" + node.id +
                                   "`) inside a list template is deferred; wrap it in an element");
        }
        break;
    case ir::NodeKind::Component:
        // A component instance owns its own state and is never inlined; its host node and the
        // caller's slot content are emitted by the composition path. Inside a list template the
        // instance is one record of the item (docs/design/compiler-node-state.md section 3).
        return emit_component_node(writer, node, state, variable, error);
    case ir::NodeKind::Slot:
        return fail(error, "a Slot (`" + node.id +
                               "`) is a child position, not a node the build() tree creates");
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
    const ListTemplate* virtual_collection = nullptr;
    if (node.kind == ir::NodeKind::VirtualList || node.kind == ir::NodeKind::VirtualGrid) {
        if (state.plan == nullptr) {
            return fail(error,
                        "the virtual collection `" + node.id +
                            "` requires the dynamic plan; this is an internal codegen error");
        }
        virtual_collection = state.plan->virtual_collection(node_ordinal);
        if (virtual_collection == nullptr) {
            return fail(error, "the virtual collection `" + node.id + "` is missing from the plan");
        }
        // The item window is attached to this fixed container node by apply_virtual_<n>().
        writer.line("this->" + item_table_prefix(*virtual_collection) + "_ = " + variable +
                    ".get();");
    }
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
    // `part` is the component library's styling API (ADR-047 item 3): a static list of names the
    // host's `::part()` selectors can target. It is never dynamic, like `role`.
    if (const ir::Expr* part_prop = prop("part")) {
        const std::optional<std::string_view> text = literal_string(part_prop);
        if (!text) {
            return fail(error, "part: a part list is a static string literal");
        }
        writer.line(variable + "->set_part(" + literal_text(ir::LiteralValue{std::string(*text)}) +
                    ");");
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
    if (node.kind == ir::NodeKind::Path) {
        // `Path.label` is the accessible name; `aria-label` is the channel the accessibility tree
        // resolves it from (accessibility-v1 section 2 name precedence).
        if (!emit_vector_label(writer, node, variable, error)) {
            return false;
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
    const bool collection_node =
        node.kind == ir::NodeKind::VirtualList || node.kind == ir::NodeKind::VirtualGrid;
    for (const auto& [jsx_name, target] : node.events) {
        (void)target;
        if (collection_node && (jsx_name == "onScroll" || jsx_name == "onEndReached")) {
            // Collection-level events are dispatched by the generated scroll_virtual_<n> entry
            // point (emit_virtual_scroll.cpp); they are not scene event bindings.
            continue;
        }
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
        if (adapter->size_change) {
            if (item_builder) {
                return fail(error, "`onSizeChange` inside a list template is not supported yet "
                                   "(a realized row would need its own observation)");
            }
            emit_size_observation_binding(writer, variable, *adapter);
            continue;
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
    // `tabIndex`/`autoFocus` on a `View` or `Text` (issue #226). Lowering validated the domain: the
    // literal is 0 (joins the Tab order) or -1 (programmatic and pointer focus only), and the node
    // has a role and a name.
    if (const ir::Expr* tab_index = prop("tabIndex");
        tab_index != nullptr && tab_index->kind == ir::ExprKind::Literal) {
        if (const auto* value = std::get_if<std::int64_t>(&tab_index->literal)) {
            writer.line(variable + "->set_focusable(true);");
            writer.line(variable + "->set_tab_index(" + std::to_string(*value) + ");");
        }
    }
    if (const ir::Expr* auto_focus = prop("autoFocus");
        auto_focus != nullptr && auto_focus->kind == ir::ExprKind::Literal) {
        if (const auto* value = std::get_if<bool>(&auto_focus->literal);
            value != nullptr && *value) {
            writer.line(variable + "->set_auto_focus(true);");
        }
    }
    // `focusTrap`/`focusGroup` (issue #236): scene-node flags the focus manager reads.
    if (const ir::Expr* trap = prop("focusTrap");
        trap != nullptr && trap->kind == ir::ExprKind::Literal) {
        if (const auto* value = std::get_if<bool>(&trap->literal); value != nullptr && *value) {
            writer.line(variable + "->set_focus_trap(true);");
        }
    }
    if (const ir::Expr* typeahead = prop("typeahead");
        typeahead != nullptr && typeahead->kind == ir::ExprKind::Literal) {
        if (const auto* value = std::get_if<bool>(&typeahead->literal);
            value != nullptr && *value) {
            writer.line(variable + "->set_typeahead(true);");
        }
    }
    if (const ir::Expr* group = prop("focusGroup");
        group != nullptr && group->kind == ir::ExprKind::Literal) {
        if (const auto* axis = std::get_if<std::string>(&group->literal)) {
            std::string name = *axis == "horizontal" ? "Horizontal"
                               : *axis == "vertical" ? "Vertical"
                                                     : "Both";
            writer.line(variable + "->set_focus_group(tessera::ui::Node::FocusGroup::" + name +
                        ");");
        }
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
    // Inline spans (issue #229): the parent's payload holds the concatenated text plus one
    // (span node id, byte range) entry per child. The entries are emitted while the children are
    // created (their ids are only readable then), so this payload is set after the child loop.
    std::string span_paint;
    std::vector<std::pair<std::size_t, std::size_t>> span_ranges;
    const bool inline_spans =
        node.kind == ir::NodeKind::Text && paint_expr == nullptr && !node.children.empty();
    if (inline_spans) {
        std::string combined;
        for (const ir::Node& child : node.children) {
            const std::optional<std::string> part =
                child.text ? static_text_value(child.text.get()) : std::nullopt;
            if (!part) {
                return fail(error, "inline span `" + child.id +
                                       "` needs a literal string text (elements-basic.md 2.1)");
            }
            span_ranges.emplace_back(combined.size(), combined.size() + part->size());
            combined += *part;
        }
        span_paint = "paint_" + variable;
        writer.line("tessera::ui::PaintData " + span_paint + ";");
        writer.line(span_paint + ".text = " + literal_text(ir::LiteralValue{std::move(combined)}) +
                    ";");
    }
    if (inline_spans) {
        // The payload is set after the children exist; see the child loop.
    } else if (const std::optional<std::string> paint_text = static_text_value(paint_expr)) {
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
    if (node.kind == ir::NodeKind::Path) {
        // Vector payload: the inline `d`/`viewBox` are synthesized into an embedded SVG asset,
        // which the paint pass decodes and rasterizes through the existing vector chain
        // (docs/design/compiler.md section 11).
        if (!emit_vector_paint(writer, node, state.component.name, node_ordinal, variable, error)) {
            return false;
        }
    }
    if (state.plan != nullptr) {
        const DynamicGroup* group = state.plan->group(node_ordinal);
        if (group != nullptr) {
            if (state.item_out.empty()) {
                writer.line("this->children_parent_" + std::to_string(node_ordinal) +
                            "_ = " + variable + ".get();");
            } else {
                writer.line(state.item_out + ".group_parent_" + std::to_string(node_ordinal) +
                            " = " + variable + ".get();");
            }
        }
    }
    for (std::size_t child_index = 0; child_index < node.children.size(); ++child_index) {
        const ir::Node& child = node.children[child_index];
        const unsigned child_ordinal = state.next_id;
        const DynamicChildKind kind = state.plan != nullptr
                                          ? state.plan->child_kind(node_ordinal, child_ordinal)
                                          : DynamicChildKind::Static;
        if (child.kind == ir::NodeKind::Slot) {
            // Section 3.4: the callee's slot position. The caller's nodes were already created and
            // handed over through the build() parameter, so attaching them is all that happens
            // here.
            if (state.slot_variable.empty()) {
                return fail(error, "a Slot (`" + child.id +
                                       "`) needs the component's `children` prop declaration");
            }
            if (kind != DynamicChildKind::Static) {
                return fail(error,
                            "a Slot (`" + child.id + "`) cannot be a conditional or list child");
            }
            writer.line("// " + child.id + " (the caller's slot content attaches here)");
            writer.open("for (auto& slot_node : " + state.slot_variable + ")");
            writer.line(variable + "->add_child(std::move(slot_node));");
            writer.close("");
            ++state.next_id;
            ++state.next_var;
            continue;
        }
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
            if (state.item_out.empty()) {
                writer.line("this->" + branch_member(child_ordinal, "owned_") + " = std::move(" +
                            child_variable + ");");
            } else {
                writer.line(state.item_out + ".branch_" + std::to_string(child_ordinal) +
                            "_owned = std::move(" + child_variable + ");");
            }
        } else {
            writer.line(variable + "->add_child(std::move(" + child_variable + "));");
        }
        if (!span_paint.empty() && child.kind == ir::NodeKind::Text &&
            child_index < span_ranges.size()) {
            const auto& [begin, end] = span_ranges[child_index];
            writer.line(span_paint + ".spans.push_back(tessera::ui::PaintSpan{" +
                        "static_cast<std::uint64_t>(" + child_variable + "->id()), " +
                        std::to_string(begin) + "u, " + std::to_string(end) + "u});");
        }
    }
    if (!span_paint.empty()) {
        writer.line(variable + "->set_paint_data(std::move(" + span_paint + "));");
    }
    if (virtual_collection != nullptr) {
        writer.line("// " + node.item_template->id + " is the virtual item template: build_item_" +
                    std::to_string(virtual_collection->ordinal) +
                    "() builds one realized item (bounded window table)");
        state.next_id = virtual_collection->subtree_end;
        state.next_var = virtual_collection->subtree_end;
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
