// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <string>

// ADR-051 implementation note (issue #834): the generated field-wise FNV-1a hash of a record, used
// by the prop hash guards of `apply_props()` and by the per-item content pass. Split from
// `emit_types_header.cpp` so both files stay under the soft line limit and the hash's one contract
// (the shared `tessera::ui::effect_hash` helpers, no allocation) has a single owner.
namespace tessera::compiler::codegen::internal {

// The definition of `hash_value(const R&)` for one record. Reads exactly the struct's members, in
// declaration order, recursing through nested records and `list`/`optional` members. False with
// `error` set when a member type has no generated hash (the IR validator rejects those shapes
// first; this guards a hand-built module).
[[nodiscard]] bool emit_record_hash_definition(CppWriter& writer, const ir::RecordType& record,
                                               const ir::IrModule& module, std::string& error);

} // namespace tessera::compiler::codegen::internal
