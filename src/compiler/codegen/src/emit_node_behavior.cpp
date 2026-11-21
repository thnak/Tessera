// SPDX-License-Identifier: MIT

// How one scene node *behaves* (docs/design/compiler.md section 11): its scene event bindings, the
// controlled-text-field wiring, refs, focusability and focus flags, scroll axis flags, and the
// paint payload (text, inline spans, vector assets) plus the dynamic child-set and item-template
// bookkeeping the child loop and apply_state() depend on. What the node *is* lives in
// emit_node_attributes.cpp; the order both files run in is the call list in emit_node.cpp.

#include "emit_controlled.hpp"
#include "emit_layout_observe.hpp"
#include "emit_node_groups.hpp"
#include "emit_vector.hpp"
#include "interpolation.hpp"

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

void emit_controlled_lookup(NodeEmitContext& context) {
    for (const ControlledInput& input : context.state.controlled_inputs) {
        if (input.ordinal == context.ordinal) {
            context.controlled_input = &input;
            return;
        }
    }
}

bool emit_event_bindings(NodeEmitContext& context) {
    const bool collection_node = context.node.kind == ir::NodeKind::VirtualList ||
                                 context.node.kind == ir::NodeKind::VirtualGrid;
    for (const auto& [jsx_name, target] : context.node.events) {
        (void)target;
        if (collection_node && (jsx_name == "onScroll" || jsx_name == "onEndReached")) {
            // Collection-level events are dispatched by the generated scroll_virtual_<n> entry
            // point (emit_virtual_scroll.cpp); they are not scene event bindings.
            continue;
        }
        if (context.controlled_input != nullptr && jsx_name == "onChange") {
            // Wired through TextChangeBinding below, not the generic scene-event adapter.
            continue;
        }
        const EventAdapter* adapter = nullptr;
        for (const EventAdapter& candidate : context.state.adapters) {
            if (candidate.ordinal == context.ordinal && candidate.jsx_name == jsx_name) {
                adapter = &candidate;
                break;
            }
        }
        if (adapter == nullptr) {
            return fail(context.error, "event `" + jsx_name + "` has no adapter");
        }
        if (adapter->size_change) {
            if (context.item_builder) {
                return fail(context.error, "`onSizeChange` inside a list template is not supported "
                                           "yet (a realized row would need its own observation)");
            }
            emit_size_observation_binding(context.writer, context.variable, *adapter);
            continue;
        }
        context.writer.line(context.variable + "->bind_event(" + adapter->event_type + ", &" +
                            adapter->function + ", this);");
    }
    return true;
}

void emit_controlled_input_wiring(NodeEmitContext& context) {
    if (context.controlled_input != nullptr) {
        emit_controlled_wiring(context.writer, context.variable, *context.controlled_input);
    }
}

void emit_ref_assignments(NodeEmitContext& context) {
    for (const ir::Ref& ref : context.state.component.refs) {
        if (ref.target != context.node.id) {
            continue;
        }
        context.writer.line("this->" + member_name(ref.id) + " = " + context.variable + ".get();");
        // `useMergedRefs` (issue #630): one assignment fills every owner the merged ref names, so a
        // component that both forwards a ref and keeps an internal one needs one `ref` attribute.
        for (const std::string& alias : ref.aliases) {
            context.writer.line("this->" + member_name(alias) + " = " + context.variable +
                                ".get();");
        }
    }
}

void emit_focusability(NodeEmitContext& context) {
    if (context.node.kind == ir::NodeKind::Button || context.node.kind == ir::NodeKind::TextInput ||
        context.node.kind == ir::NodeKind::TextArea) {
        context.writer.line(context.variable + "->set_focusable(true);");
    }
    // `tabIndex`/`autoFocus` on a `View` or `Text` (issue #226). Lowering validated the domain: the
    // literal is 0 (joins the Tab order) or -1 (programmatic and pointer focus only), and the node
    // has a role and a name.
    if (const ir::Expr* tab_index = context.prop("tabIndex");
        tab_index != nullptr && tab_index->kind == ir::ExprKind::Literal) {
        if (const auto* value = std::get_if<std::int64_t>(&tab_index->literal)) {
            context.writer.line(context.variable + "->set_focusable(true);");
            context.writer.line(context.variable + "->set_tab_index(" + std::to_string(*value) +
                                ");");
        }
    }
    if (const ir::Expr* auto_focus = context.prop("autoFocus");
        auto_focus != nullptr && auto_focus->kind == ir::ExprKind::Literal) {
        if (const auto* value = std::get_if<bool>(&auto_focus->literal);
            value != nullptr && *value) {
            context.writer.line(context.variable + "->set_auto_focus(true);");
        }
    }
    // `focusTrap`/`focusGroup` (issue #236): scene-node flags the focus manager reads.
    if (const ir::Expr* trap = context.prop("focusTrap");
        trap != nullptr && trap->kind == ir::ExprKind::Literal) {
        if (const auto* value = std::get_if<bool>(&trap->literal); value != nullptr && *value) {
            context.writer.line(context.variable + "->set_focus_trap(true);");
        }
    }
    if (const ir::Expr* typeahead = context.prop("typeahead");
        typeahead != nullptr && typeahead->kind == ir::ExprKind::Literal) {
        if (const auto* value = std::get_if<bool>(&typeahead->literal);
            value != nullptr && *value) {
            context.writer.line(context.variable + "->set_typeahead(true);");
        }
    }
    if (const ir::Expr* group = context.prop("focusGroup");
        group != nullptr && group->kind == ir::ExprKind::Literal) {
        if (const auto* axis = std::get_if<std::string>(&group->literal)) {
            const std::string name = *axis == "horizontal" ? "Horizontal"
                                     : *axis == "vertical" ? "Vertical"
                                                           : "Both";
            context.writer.line(context.variable +
                                "->set_focus_group(tessera::ui::Node::FocusGroup::" + name + ");");
        }
    }
}

void emit_scroll_flags(NodeEmitContext& context) {
    if (context.node.kind != ir::NodeKind::Scroll) {
        return;
    }
    std::string axis = "y";
    const auto found = context.node.props.find("axis");
    if (found != context.node.props.end() && found->second &&
        found->second->kind == ir::ExprKind::Literal) {
        if (const auto* text = std::get_if<std::string>(&found->second->literal)) {
            axis = *text;
        }
    }
    context.writer.line(context.variable + "->set_scrollable_x(" +
                        (axis == "x" || axis == "both" ? "true" : "false") + ");");
    context.writer.line(context.variable + "->set_scrollable_y(" +
                        (axis == "y" || axis == "both" ? "true" : "false") + ");");
}

bool emit_paint_payload(NodeEmitContext& context) {
    const ir::Expr* paint_expr = context.node.text.get();
    if (paint_expr == nullptr && context.controlled_input != nullptr) {
        paint_expr = context.controlled_input->value;
    }
    // Inline spans (issue #229): the parent's payload holds the concatenated text plus one
    // (span node id, byte range) entry per child. The entries are emitted while the children are
    // created (their ids are only readable then), so this payload is set after the child loop.
    const bool inline_spans = context.node.kind == ir::NodeKind::Text && paint_expr == nullptr &&
                              !context.node.children.empty();
    if (inline_spans) {
        std::string combined;
        for (const ir::Node& child : context.node.children) {
            const std::optional<std::string> part =
                child.text ? static_text_value(child.text.get()) : std::nullopt;
            if (!part) {
                return fail(context.error,
                            "inline span `" + child.id +
                                "` needs a literal string text (elements-basic.md 2.1)");
            }
            context.span_ranges.emplace_back(combined.size(), combined.size() + part->size());
            combined += *part;
        }
        context.span_paint = "paint_" + context.variable;
        context.writer.line("tessera::ui::PaintData " + context.span_paint + ";");
        context.writer.line(context.span_paint +
                            ".text = " + literal_text(ir::LiteralValue{std::move(combined)}) + ";");
    } else if (const std::optional<std::string> paint_text = static_text_value(paint_expr)) {
        // D9 paint payload: text is visual only (layout/wrap stay paint-side).
        const std::string paint = "paint_" + context.variable;
        context.writer.line("tessera::ui::PaintData " + paint + ";");
        context.writer.line(
            paint + ".text = " + literal_text(ir::LiteralValue{std::string(*paint_text)}) + ";");
        context.writer.line(context.variable + "->set_paint_data(std::move(" + paint + "));");
        if (context.controlled_input != nullptr) {
            context.writer.line("this->" + context.controlled_input->editor_member + ".set_text(" +
                                literal_text(ir::LiteralValue{std::string(*paint_text)}) + ");");
        }
    } else if (context.text_site != nullptr) {
        // The per-item content pass paints this node from the loop item/index values.
        context.writer.line(context.state.text_out + "[" + std::to_string(context.text_site->site) +
                            "] = " + context.variable + ".get();");
    } else if (context.dynamic) {
        const std::string suffix = std::to_string(context.ordinal);
        context.writer.line("this->text_node_" + suffix + "_ = " + context.variable + ".get();");
    }
    return true;
}

bool emit_path_paint(NodeEmitContext& context) {
    if (context.node.kind != ir::NodeKind::Path) {
        return true;
    }
    // Vector payload: the inline `d`/`viewBox` are synthesized into an embedded SVG asset, which
    // the paint pass decodes and rasterizes through the existing vector chain
    // (docs/design/compiler.md section 11).
    return emit_vector_paint(context.writer, context.node, context.state.component.name,
                             context.ordinal, context.variable, context.error);
}

void emit_dynamic_group_parent(NodeEmitContext& context) {
    if (context.state.plan == nullptr) {
        return;
    }
    if (context.state.plan->group(context.ordinal) == nullptr) {
        return;
    }
    if (context.state.item_out.empty()) {
        context.writer.line("this->children_parent_" + std::to_string(context.ordinal) +
                            "_ = " + context.variable + ".get();");
        return;
    }
    context.writer.line(context.state.item_out + ".group_parent_" +
                        std::to_string(context.ordinal) + " = " + context.variable + ".get();");
}

void emit_span_paint_data(NodeEmitContext& context) {
    if (!context.span_paint.empty()) {
        context.writer.line(context.variable + "->set_paint_data(std::move(" + context.span_paint +
                            "));");
    }
}

void emit_virtual_item_template_skip(NodeEmitContext& context) {
    if (context.virtual_collection == nullptr) {
        return;
    }
    context.writer.line("// " + context.node.item_template->id +
                        " is the virtual item template: build_item_" +
                        std::to_string(context.virtual_collection->ordinal) +
                        "() builds one realized item (bounded window table)");
    context.state.next_id = context.virtual_collection->subtree_end;
    context.state.next_var = context.virtual_collection->subtree_end;
}

} // namespace tessera::compiler::codegen::internal
