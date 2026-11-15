// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <optional>
#include <string>
#include <string_view>

// ADR-051 slice 4: the generated C++ form of the module's record type table
// (docs/specs/ir-v1-expr.md section 10). One aggregate `struct` per record, in `types` order, in
// the per-module `<module>_types.hpp`.
namespace tessera::compiler::codegen::internal {

// The declared record `name` of `module`, or null. A record name in an IR type is what selects the
// aggregate form of a prop default or a `useState` initial.
[[nodiscard]] const ir::RecordType* find_record_type(const ir::IrModule& module,
                                                     std::string_view name);

// The single type inside any `list<>`/`optional<>` wrappers (`optional<Size>` -> `Size`).
[[nodiscard]] std::string_view innermost_record_name(std::string_view ir_type);

// True when any prop or state initial of `component` has a declared record type, so its generated
// header has to include `<module>_types.hpp`. False for every module without records, which is what
// keeps the existing goldens byte-identical.
[[nodiscard]] bool uses_record_types(const ir::Component& component, const ir::IrModule& module);

// Emits `<module>_types.hpp`. False with `error` set when a member type has no C++ form (the IR
// validator rejects that shape first, so this is a defence against a hand-built module).
[[nodiscard]] bool emit_types_header(const ir::IrModule& module, GeneratedFile& file,
                                     std::string& error);

} // namespace tessera::compiler::codegen::internal
