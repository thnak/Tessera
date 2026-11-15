// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "emit_dynamic.hpp"
#include "emit_node.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The emission groups of one scene node (docs/design/compiler.md section 11), split out of
// emit_node.cpp so the node walk stays readable: emit_node() validates the node, creates it, runs
// the groups in the documented per-node order and then emits the children. They share the per-node
// values through NodeEmitContext instead of taking the writer, node, walk state, variable name and
// error as five parameters each, which keeps every group's signature a single argument.
//
// The definitions split by role: emit_node_attributes.cpp emits what the node *is* (validation,
// creation, kind, ids, aria, inline style) and emit_node_behavior.cpp how it *behaves* (events,
// controlled wiring, refs, focus, scroll, paint payload).
namespace tessera::compiler::codegen::internal {

// The per-node values the emission groups read. `id`, `ordinal`, `text_site`, `dynamic`, `variable`
// and `item_builder` are filled in by the prelude and the creation group, before any other group
// runs.
struct NodeEmitContext {
    // How the prelude leaves the node: `Proceed` runs the emission groups, `Emitted` means another
    // path already emitted it (a component instance builds its own host node), and `Failed` means
    // `error` is set.
    enum class Outcome : std::uint8_t { Proceed, Emitted, Failed };

    NodeEmitContext(CppWriter& emit_writer, const ir::Node& emit_node, NodeEmitState& emit_state,
                    std::string& emit_variable, std::string& emit_error)
        : writer(emit_writer),
          node(emit_node),
          state(emit_state),
          variable(emit_variable),
          error(emit_error) {}

    CppWriter& writer;
    const ir::Node& node;
    NodeEmitState& state;
    std::string& variable; // the generated `node_N` variable of this node
    std::string& error;    // the rejection message of a node codegen cannot emit

    std::string id;       // the node id expression (node_id_text)
    unsigned ordinal = 0; // pre-order ordinal every collector keys on
    // The item-builder text site of this node, or nullptr when it is not interpolated template
    // text.
    const ListTextSite* text_site = nullptr;
    bool dynamic = false;      // the text is painted by apply_state(), not a static payload
    bool item_builder = false; // this node is emitted inside a list-item builder
    // The generated control of this node, when it is a controlled text field.
    const ControlledInput* controlled_input = nullptr;
    // The item template of a VirtualList/VirtualGrid node, resolved from the dynamic plan.
    const ListTemplate* virtual_collection = nullptr;

    // Inline spans (issue #229): the parent Text's concatenated payload plus one byte range per
    // text child. The ranges are only readable while the children are created, so the paint group
    // records them here and the child loop turns each one into a `PaintSpan`.
    std::string span_paint;
    std::vector<std::pair<std::size_t, std::size_t>> span_ranges;

    // The node's prop `name`, or nullptr when it has none.
    [[nodiscard]] const ir::Expr* prop(std::string_view name) const;

    // True when `name` is re-applied per state by apply_state(), so the node keeps a borrowed
    // value.
    [[nodiscard]] bool is_dynamic_metadata(MetadataField field, std::string_view name) const;
};

// Rejects the node kinds and expressions codegen cannot emit; returns Emitted for a component
// instance, whose host node and slot content the composition path emits.
[[nodiscard]] NodeEmitContext::Outcome emit_node_prelude(NodeEmitContext& context);

// Emits the node comment and the `std::make_unique<tessera::ui::Node>` creation, and records the
// id expression, ordinal, text site, dynamic-text flag and generated variable name.
[[nodiscard]] bool emit_node_creation(NodeEmitContext& context);

// `set_element_kind`, the virtual collection's item-table slot, and the pointers apply_state() uses
// to refresh the dynamic metadata of this node.
[[nodiscard]] bool emit_element_kind(NodeEmitContext& context);

void emit_dynamic_metadata_pointers(NodeEmitContext& context);

// The element id, className, automationId, role and `part` list.
[[nodiscard]] bool emit_element_metadata(NodeEmitContext& context);

// The `aria-*` attributes and, for a `Path`, its accessible name (`label`).
[[nodiscard]] bool emit_aria_attributes(NodeEmitContext& context);

// The literal inline style declarations; a non-literal value is left to apply_state().
void emit_inline_style(NodeEmitContext& context);

// Resolves the generated control of this node; the events and paint payload consult it.
void emit_controlled_lookup(NodeEmitContext& context);

// The scene event adapters and the `onSizeChange` observation.
[[nodiscard]] bool emit_event_bindings(NodeEmitContext& context);

// The editor, runtime and change-binding members of a controlled text field.
void emit_controlled_input_wiring(NodeEmitContext& context);

// The `useRef` targets that point at this node.
void emit_ref_assignments(NodeEmitContext& context);

// Focusable kinds and the focus flags (tabIndex, autoFocus, focusTrap, typeahead, focusGroup).
void emit_focusability(NodeEmitContext& context);

// The scroll axis flags of a `Scroll` node.
void emit_scroll_flags(NodeEmitContext& context);

// The paint payload: the inline-span setup, a static text payload, the per-item text site, or the
// dynamic text slot apply_state() paints.
[[nodiscard]] bool emit_paint_payload(NodeEmitContext& context);

// The vector paint payload of a `Path` node.
[[nodiscard]] bool emit_path_paint(NodeEmitContext& context);

// The generated parent pointer a dynamic child set attaches its children to.
void emit_dynamic_group_parent(NodeEmitContext& context);

// Hands the inline-span payload to the node; runs after the children exist.
void emit_span_paint_data(NodeEmitContext& context);

// Skips the virtual item template subtree: build_item_<n>() builds one realized item per item.
void emit_virtual_item_template_skip(NodeEmitContext& context);

} // namespace tessera::compiler::codegen::internal
