// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "emit_dirty.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Component prop and callback-prop instantiation (docs/design/compiler.md section 11,
// docs/specs/component-model-v1.md section 3). A `Component` node passes its props to the
// referenced component's declared prop records: a value prop assigns the generated member of the
// instance before it is built, and a record-typed (callback) prop is bound through the node's
// events. No library component gets a special path; `Field` is an ordinary declared component.
namespace tessera::compiler::codegen::internal {

// True for an IR type with a plain generated C++ value form (a primitive, `list<T>`, `optional<T>`,
// or a declared record of `records`). False for a record name that is not declared: a record-typed
// prop is then a callback prop, which is not a value and must be bound through `node.events`
// (docs/specs/component-model-v1.md section 3).
[[nodiscard]] bool is_value_prop_type(std::string_view ir_type,
                                      std::span<const ir::RecordType> records = {});

// The generated C++ type of a callback prop: `std::function<void(const <Record>&)>` for a prop
// whose IR type names a scene payload record, and `std::function<void()>` for the payload-free
// `callback` prop of a library component (`Form`'s `validate`/`onSubmit`). Empty when the type has
// no C++ mapping.
[[nodiscard]] std::string callback_prop_type(std::string_view ir_type);

// How `apply_props()` guards one prop assignment: a 64-bit FNV-1a value hash for a printable scalar
// or list-of-printable prop (the same allocation-free change detection apply_state() uses), a
// one-shot bound flag for a callback (the lambda is built once), or no guard when the type has no
// hashable form. A record value is `None`: ADR-051 section 5 does not generate equality or hashing
// for a record in this slice, so a record prop is assigned on every refresh (as a list would be).
enum class PropGuard : std::uint8_t { None, Value, Callback };

[[nodiscard]] PropGuard prop_guard(std::string_view ir_type,
                                   std::span<const ir::RecordType> records = {});

// The generated caller-struct member that stores a prop's guard. `_hash_` is a `std::uint64_t`,
// `_bound_` is a `bool`.
[[nodiscard]] std::string prop_guard_member(unsigned ordinal, std::string_view name);
[[nodiscard]] std::string prop_bound_member(unsigned ordinal, std::string_view name);

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
                                    std::span<const ir::RecordType> records, const DirtyPlan& dirty,
                                    const std::vector<ComponentInstance>& instances,
                                    std::string& error);

// ADR-066 item 5 (issue #802): the same walk, plus the provider push — for every instance whose
// component consumes a context this component provides, `apply_props()` pushes the provider's
// value into that instance's consumer setter. `module` resolves the instance's component to its
// `context:` environment entries.
[[nodiscard]] bool emit_apply_props_with_context(CppWriter& writer, const ir::Component& owner,
                                                 const ir::IrModule& module,
                                                 std::span<const ir::RecordType> records,
                                                 const DirtyPlan& dirty,
                                                 const std::vector<ComponentInstance>& instances,
                                                 std::string& error);

// ADR-066 item 5 (issue #802): the provider nodes of one component's tree, by context name
// (`emit_provider_push.cpp`). False when the component provides no context.
[[nodiscard]] bool collect_providers(const ir::Component& owner,
                                     std::map<std::string, const ir::Node*>& out);

// The provider half of `apply_props()`: one push per instance under a provider of a context it
// consumes, into the instance's consumer setter. Emits nothing when the component provides no
// context.
[[nodiscard]] bool emit_provider_pushes(CppWriter& writer, const ir::Component& owner,
                                        const ir::IrModule& module,
                                        const std::map<std::string, const ir::Node*>& provided,
                                        const std::vector<ComponentInstance>& instances,
                                        std::string& error);

// One per-item instance's props (docs/design/compiler-node-state.md section 3). `record` names the
// item record (`tessera_out`) and `field` the instance field in it (`tessera_out.child_7`).
// `initial` emits the values the instance is built with (no guard; a callback lambda is bound once
// and captures `this`, the item's owner component); otherwise it emits the hash-guarded refresh the
// per-item content pass runs. A prop value may read the loop locals (`locals`).
[[nodiscard]] bool emit_item_instance_props(CppWriter& writer, const ir::Component& owner,
                                            const ComponentInstance& instance,
                                            std::string_view record, std::string_view field,
                                            const LocalBindings& locals, bool initial,
                                            std::string& error);

} // namespace tessera::compiler::codegen::internal
