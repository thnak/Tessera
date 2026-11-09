// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Event payload mapping and generated dispatch adapters (docs/design/compiler.md section 11).
// Kept out of emit_component.cpp so each codegen translation unit stays under the file limit.
namespace tessera::compiler::codegen::internal {

// One schema payload record -> C++ record and its conversion function.
struct PayloadRecord {
    std::string_view record;
    std::string_view cpp_type;
    std::string_view converter;
};

[[nodiscard]] const PayloadRecord* find_payload(std::string_view record);

// `const tessera::ui::ClickEvent& e` when the first IR param names a payload record, otherwise
// `const tessera::ui::Event& event`. `typed` says which form was chosen; nullopt means the IR
// names a payload record that has no C++ mapping.
[[nodiscard]] std::optional<std::string> payload_signature(const std::vector<ir::Param>& params,
                                                           bool& typed, std::string& param_name);

// `handler::1` -> `handler_1`: the generated member name of an IR record id.
[[nodiscard]] std::string method_name(std::string_view id);

// C++ type text for an IR type used as a return type: a payload record maps to its C++ record,
// otherwise the primitive mapping. Empty when there is no C++ form.
[[nodiscard]] std::string handler_type(std::string_view ir_type);

// A free adapter function the generated build() installs with Node::bind_event.
struct EventAdapter {
    unsigned ordinal = 0;   // pre-order node number, matching the generated `node_N` variable
    std::string jsx_name;   // node.events key (`onClick`)
    std::string event_type; // `tessera::ui::EventType::PointerUp`
    std::string function;   // `event_adapter_2_onClick`
    std::string call;       // `component->handler_1(tessera::ui::click_event(event));`
};

// Walks the node tree in pre-order, assigning the same ordinals as emit_node, and resolves every
// node event to its handler method, payload type and EventType. Sets `error` on an unsupported
// JSX event name or an unresolved handler/binding target.
[[nodiscard]] bool collect_event_adapters(const ir::Node& node, const ir::Component& component,
                                          const std::vector<const ir::Binding*>& bindings,
                                          unsigned& ordinal, std::vector<EventAdapter>& adapters,
                                          std::string& error);

// Emits the adapter functions in an anonymous namespace (deterministic IR order).
void emit_event_adapters(CppWriter& writer, std::string_view component_name,
                         const std::vector<EventAdapter>& adapters);

} // namespace tessera::compiler::codegen::internal
