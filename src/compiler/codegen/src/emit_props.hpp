// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <string>
#include <string_view>
#include <vector>

// Component prop and callback-prop instantiation (docs/design/compiler.md section 11,
// docs/specs/component-model-v1.md section 3). A `Component` node passes its props to the
// referenced component's declared prop records: a value prop assigns the generated member of the
// instance before it is built, and a record-typed (callback) prop is bound through the node's
// events. No library component gets a special path; `Field` is an ordinary declared component.
namespace tessera::compiler::codegen::internal {

// True for an IR type with a plain generated C++ value form (a primitive, `list<T>` or
// `optional<T>`), false for a record type. A record-typed prop is a callback prop: it is not a
// value, so it must be bound through `node.events` (docs/specs/component-model-v1.md section 3).
[[nodiscard]] bool is_value_prop_type(std::string_view ir_type);

// The generated C++ type of a callback prop: `std::function<void(const <Record>&)>` for a prop
// whose IR type names a scene payload record. Empty when the record has no C++ mapping.
[[nodiscard]] std::string callback_prop_type(std::string_view ir_type);

// Resolves and validates the props and callback props of one instance against the target
// component's declared prop records. Unknown prop names, a value passed for a callback prop (or a
// callback bound for a value prop), a literal of the wrong type, and an unbound/undeclared handler
// are codegen errors. The result is deterministic: declared prop order first, then events order.
[[nodiscard]] bool collect_instance_props(const ir::IrModule& module, const ir::Component& owner,
                                          const ir::Node& node, const ir::Component& target,
                                          ComponentInstance& instance, std::string& error);

// Emits `<Name>::apply_props()`: one assignment per value prop and one lambda per callback prop for
// every component instance the component owns, in instance order. Returns false with `error` when a
// prop value has no C++ form. Nothing is emitted when no instance has prop bindings.
[[nodiscard]] bool emit_apply_props(CppWriter& writer, const ir::Component& owner,
                                    const std::vector<ComponentInstance>& instances,
                                    std::string& error);

} // namespace tessera::compiler::codegen::internal
