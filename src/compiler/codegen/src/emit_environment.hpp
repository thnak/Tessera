// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <string>

// `environment` hook values in the generated base (docs/specs/jsx-subset-v1.md section 7.1, issue
// #692): `useBreakpoint()`, `useTheme()` and `useThemeState()`.
//
// An environment value is engine-owned, not component-owned: the component reads it, never writes
// it (except through a writable hook's setter, which forwards to the host). So the generated base
// holds one field per value, a `set_environment_<name>()` the host calls when the value changes,
// and an accessor the developer class reads. The host is the only writer, which is what keeps the
// value and the stylesheet's own `@media` answer in step: both come from the same `MediaContext`.
namespace tessera::compiler::codegen::internal {

// True when the component reads at least one environment value, so the base needs the surface.
[[nodiscard]] bool has_environment(const ir::Component& component);

// The private fields and the public setter/accessor declarations.
void emit_environment_declarations(CppWriter& writer, const ir::Component& component,
                                   const ir::IrModule& module);

// The setter and accessor definitions.
[[nodiscard]] bool emit_environment_definitions(CppWriter& writer, const ir::Component& component,
                                                const ir::IrModule& module, std::string& error);

// The C++ type of one environment value (`std::string` for the size class and the theme, `bool` for
// a media query). ADR-066 item 5 (issue #802): a `context:` provider's type is the context's, which
// may be a declared record, so the type table decides (`cpp_type`).
[[nodiscard]] std::string environment_cpp_type(const ir::Environment& env,
                                               std::span<const ir::RecordType> records = {});

// The initial value the field starts at, before the host pushes the real one. `compact` for the
// breakpoint (the narrow layout, the safe first frame), the built-in theme id for the theme, and
// `false` for a media query (an unevaluated query does not match). ADR-066 item 5 (issue #802): a
// `context:` provider starts at its `createContext` default, which the environment entry carries as
// its argument; a record default emits as designated aggregate initialization.
[[nodiscard]] bool environment_initial_value(const ir::Environment& env,
                                             std::span<const ir::RecordType> records,
                                             std::string& out, std::string& error);

} // namespace tessera::compiler::codegen::internal
