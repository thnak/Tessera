// SPDX-License-Identifier: MIT

// One node and its subtree -> native C++ (docs/design/compiler.md section 11). The per-node
// emission order is part of the contract, and it is the call list below, in this order:
//
//    1. emit_node_prelude         reject the node kinds and expressions codegen cannot emit
//    2. emit_node_creation        the node comment and `std::make_unique<Node>`
//    3. emit_element_kind         element kind, and the virtual collection's item table
//    4. emit_dynamic_metadata_pointers  the node pointers apply_state() refreshes
//    5. emit_element_metadata     id, className, automationId, role, part
//    6. emit_aria_attributes      aria-*, and `Path.label` as the accessible name
//    7. emit_controlled_lookup    the controlled text field, when this node is one
//    8. emit_inline_style         the literal inline style declarations
//    9. emit_event_bindings       scene events and `onSizeChange`
//   10. emit_controlled_input_wiring  the editor, runtime and change-binding members
//   11. emit_ref_assignments      the `useRef` targets pointing at this node
//   12. emit_focusability         focusable kinds and the focus flags
//   13. emit_scroll_flags         the scroll axis flags
//   14. emit_paint_payload        the text payload (inline spans, static, per-item, dynamic)
//   15. emit_path_paint           the vector payload of a `Path`
//   16. emit_dynamic_group_parent the parent pointer a dynamic child set attaches to
//   17. the child loop            one recursive emit_node per child, in IR order
//   18. emit_span_paint_data      the inline-span payload, once the children exist
//   19. emit_virtual_item_template_skip  the item template build_item_<n>() owns
//
// A conditional branch is a child with `condition`: it is built into its generated ownership slot
// and attached by apply_state(). A list template (a child with `loop`) is not built here; its nodes
// are produced per item by the generated build_item_<n>().

#include "emit_dynamic.hpp"
#include "emit_node.hpp"
#include "emit_node_groups.hpp"
#include "emit_suspense.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

} // namespace

bool emit_node(CppWriter& writer, const ir::Node& node, NodeEmitState& state, std::string& variable,
               std::string& error) {
    NodeEmitContext context(writer, node, state, variable, error);
    switch (emit_node_prelude(context)) {
    case NodeEmitContext::Outcome::Failed:
        return false;
    case NodeEmitContext::Outcome::Emitted:
        return true;
    case NodeEmitContext::Outcome::Proceed:
        break;
    }
    if (!emit_node_creation(context) || !emit_element_kind(context)) {
        return false;
    }
    emit_dynamic_metadata_pointers(context);
    if (!emit_element_metadata(context) || !emit_aria_attributes(context)) {
        return false;
    }
    emit_controlled_lookup(context);
    emit_inline_style(context);
    if (!emit_event_bindings(context)) {
        return false;
    }
    emit_controlled_input_wiring(context);
    emit_ref_assignments(context);
    emit_focusability(context);
    emit_scroll_flags(context);
    if (!emit_paint_payload(context) || !emit_path_paint(context)) {
        return false;
    }
    emit_dynamic_group_parent(context);
    // ADR-067 item 2: a Suspense boundary is a static node whose own child set swaps between two
    // subtrees, so it emits both into their ownership slots instead of the normal child loop.
    if (node.kind == ir::NodeKind::Suspense) {
        if (!emit_suspense_children(context)) {
            return false;
        }
        emit_span_paint_data(context);
        emit_virtual_item_template_skip(context);
        return true;
    }
    // The child loop's own node is the parent a static boundary attaches to.
    const std::string outer_parent = state.parent_variable;
    state.parent_variable = variable;
    for (std::size_t child_index = 0; child_index < node.children.size(); ++child_index) {
        const ir::Node& child = node.children[child_index];
        const unsigned child_ordinal = state.next_id;
        const DynamicChildKind kind = state.plan != nullptr
                                          ? state.plan->child_kind(context.ordinal, child_ordinal)
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
        // The child's id is read before `add_child` moves the pointer away (reading it after is a
        // null dereference).
        if (!context.span_paint.empty() && child.kind == ir::NodeKind::Text &&
            child_index < context.span_ranges.size()) {
            const auto& [begin, end] = context.span_ranges[child_index];
            writer.line(context.span_paint + ".spans.push_back(tessera::ui::PaintSpan{" +
                        "static_cast<std::uint64_t>(" + child_variable + "->id()), " +
                        std::to_string(begin) + "u, " + std::to_string(end) + "u});");
        }
        if (kind == DynamicChildKind::Conditional) {
            if (state.item_out.empty()) {
                writer.line("this->" + branch_member(child_ordinal, "owned_") + " = std::move(" +
                            child_variable + ");");
            } else {
                writer.line(state.item_out + ".branch_" + std::to_string(child_ordinal) +
                            "_owned = std::move(" + child_variable + ");");
            }
        } else if (child.kind == ir::NodeKind::Suspense) {
            // ADR-067 item 2: a static boundary attaches its own live subtree at this position, so
            // the parent must not attach anything for it.
            writer.line("// " + child.id + " (a Suspense boundary attaches its live subtree)");
        } else {
            writer.line(variable + "->add_child(std::move(" + child_variable + "));");
        }
    }
    emit_span_paint_data(context);
    emit_virtual_item_template_skip(context);
    state.parent_variable = outer_parent;
    return true;
}

} // namespace tessera::compiler::codegen::internal
