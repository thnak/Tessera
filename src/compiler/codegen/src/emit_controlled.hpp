// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <string>
#include <vector>

// Controlled TextInput/TextArea emission (docs/design/compiler.md section 11).
namespace tessera::compiler::codegen::internal {

// One controlled text field: `value`/`defaultValue` seeds PaintData text and the generated
// onChange handler is bound through the typed TextChange adapter from text_input_runtime.hpp.
struct ControlledInput {
    unsigned ordinal = 0;
    const ir::Expr* value = nullptr;
    std::string value_member;   // `text_node_N_` (dynamic value) or empty for a static literal
    std::string editor_member;  // `input_N_editor_`
    std::string runtime_member; // `input_N_runtime_`
    std::string binding_member; // `input_N_change_binding_`
    std::string adapter;        // `text_change_adapter_N`
    std::string method;         // generated handler/binding method called with the payload
    bool dynamic = false;       // value is an interpolation handled by apply_state()
};

[[nodiscard]] bool collect_controlled_inputs(const ir::Node& node, const ir::Component& component,
                                             const std::vector<const ir::Binding*>& bindings,
                                             unsigned& ordinal,
                                             std::vector<ControlledInput>& inputs,
                                             std::string& error);

void emit_controlled_fields(CppWriter& writer, const std::vector<ControlledInput>& inputs);

void emit_controlled_adapters(CppWriter& writer, std::string_view component_name,
                              const std::vector<ControlledInput>& inputs);

// Emits the runtime wiring for one node (after the node's event bindings).
void emit_controlled_wiring(CppWriter& writer, const std::string& variable,
                            const ControlledInput& input);

// Emits the editor-buffer sync after apply_state's text blocks.
void emit_controlled_apply(CppWriter& writer, const std::vector<ControlledInput>& inputs);

// Emits the editor/change-count accessors used by the integration demo and tests.
void emit_controlled_accessors(CppWriter& writer, const ir::Component& component,
                               const std::vector<ControlledInput>& inputs);

} // namespace tessera::compiler::codegen::internal
