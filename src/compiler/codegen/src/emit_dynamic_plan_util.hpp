// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <optional>
#include <string>
#include <string_view>

// Helpers of the dynamic-plan walk (docs/design/compiler.md section 11), split out of
// emit_dynamic_plan.cpp.
namespace tessera::compiler::codegen::internal::plan_detail {

// `TESSERA_CODEGEN_UNSUPPORTED: <message>` into `error`; always false, so callers `return
// fail(...)`.
bool fail(std::string& error, std::string message);

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix);

// Generated item builders bind the loop item/index by name, so a name that collides with a
// generated local, the generated id prefix or a helper the item body calls would not compile (or
// would silently resolve to the wrong object). Rejecting them keeps the generated code readable.
[[nodiscard]] bool reserved_loop_local(std::string_view name);

// The declared IR type of a component field reference (`state::rows` -> `list<string>`).
[[nodiscard]] std::optional<std::string> declared_field_type(const ir::Component& component,
                                                             std::string_view ref_id);

// The declared list type of a loop source: a list reference reads a state/prop `list<T>`; a
// `.filter(...)`/`.slice(...)` chain (ADR-066 item 3, issue #804) reads the type of the list it
// filters, because a filter keeps the element type and a slice's bounds cannot change it. One
// level of chain is followed recursively; anything else has no static list type here.
[[nodiscard]] std::optional<std::string> loop_source_type(const ir::Component& component,
                                                          const ir::Expr& source);

// `list<T>` -> `T`.
[[nodiscard]] std::optional<std::string> list_element_type(std::string_view type);

// True when `expr` reads the loop item or index (directly or as `item.field`). A key that reads
// only component state would give every item the same key, so it is rejected instead of silently
// degrading to a positional match.
[[nodiscard]] bool mentions_loop_local(const ir::Expr& expr, std::string_view item,
                                       std::string_view index);

} // namespace tessera::compiler::codegen::internal::plan_detail
