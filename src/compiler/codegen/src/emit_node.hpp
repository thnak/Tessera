// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "emit_controlled.hpp"
#include "emit_dynamic.hpp"
#include "emit_events.hpp"
#include "value_ops.hpp"

#include <string>
#include <vector>

// Node-tree emission shared by build() (the static tree plus detached conditional branches) and the
// generated list-item builders (docs/design/compiler.md section 11).
namespace tessera::compiler::codegen::internal {

// Per-walk state. `next_id` is the pre-order ordinal the collectors use; `next_var` numbers the
// generated `node_N` variables. They advance together, and a skipped list template advances both by
// its subtree size so every later ordinal stays aligned with the plan and the other collectors.
struct NodeEmitState {
    NodeEmitState(const ir::Component& emit_component,
                  const std::vector<EventAdapter>& emit_adapters,
                  const std::vector<DynamicText>& emit_dynamic_text,
                  const std::vector<DynamicMetadata>& emit_dynamic_metadata,
                  const std::vector<ControlledInput>& emit_controlled_inputs,
                  const DynamicPlan* emit_plan)
        : component(emit_component),
          adapters(emit_adapters),
          dynamic_text(emit_dynamic_text),
          dynamic_metadata(emit_dynamic_metadata),
          controlled_inputs(emit_controlled_inputs),
          plan(emit_plan) {}

    const ir::Component& component;
    const std::vector<EventAdapter>& adapters;
    const std::vector<DynamicText>& dynamic_text;
    const std::vector<DynamicMetadata>& dynamic_metadata;
    const std::vector<ControlledInput>& controlled_inputs;
    const DynamicPlan* plan = nullptr;

    // List-template item builder mode: loop locals are visible, node ids carry the item index
    // offset, and interpolated text is recorded for the per-item content pass instead of the
    // component-level dynamic-text table.
    const LocalBindings* locals = nullptr;
    const std::vector<ListTextSite>* text_sites = nullptr;
    std::string index_expr; // `tessera_id_slot` inside an item builder, empty in the static tree
    std::string text_out;   // `tessera_out.texts` inside an item builder, empty otherwise
    // The item record (`tessera_out`) an item builder writes to: conditional branch slots, dynamic
    // group parents and child component instances of the item live there instead of on the
    // component struct (docs/design/compiler-node-state.md sections 3 and 4). Empty in build().
    std::string item_out;

    // Generated form-state binding of this component, when it owns a library `Form` instance
    // (form_binding.hpp). Empty outside a form component.
    const FormPlan* forms = nullptr;

    // Component composition (emit_composition.cpp): the module supplies the referenced component
    // records, and `slot_variable` names the build() parameter that carries the caller's slot
    // content (empty in a component that does not declare `children`).
    const ir::IrModule* module = nullptr;
    std::string slot_variable;
    // The caller component's collected instances: a per-item instance's prop bindings are read from
    // here while its item builder is emitted (docs/design/compiler-node-state.md section 3).
    const std::vector<ComponentInstance>* instances = nullptr;

    unsigned next_id = 1;
    unsigned next_var = 1;
};

// Emits `node` and its subtree; `variable` receives the generated variable name of the created
// node. The caller attaches the returned node. Children are attached here, except a dynamic child
// of `node`: a conditional branch is stored in its generated ownership slot, and a list template is
// not built here at all (apply_state() builds its items from the generated item table).
[[nodiscard]] bool emit_node(CppWriter& writer, const ir::Node& node, NodeEmitState& state,
                             std::string& variable, std::string& error);

} // namespace tessera::compiler::codegen::internal
