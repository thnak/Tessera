// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Text value typing and the shared hash/append emission of the generated update path
// (docs/design/compiler.md section 11). Shared by apply_state()'s interpolated text and metadata
// and by the generated keyed-list item content pass, so both follow one formatting/hash contract.
namespace tessera::compiler::codegen::internal {

// The C++ type family a text interpolation expression produces.
enum class TextValueType : std::uint8_t { Bool, Int, Float, String };

// The text family of an IR type name, when it has a printable C++ form.
[[nodiscard]] std::optional<TextValueType> text_value_type(std::string_view ir_type);

// The declared text family of a component field reference (`state::x`, `prop::x`, `anim::x`).
// Local names are handled by infer_text_type; this covers the component scope only.
//
// `records` is the module's record type table and is what makes a dotted member read printable
// (#827): `state::state.count` names a record-typed cell, and the record itself has no printable
// form, so the lookup has to reach the *member's* declared type. An empty table keeps the old
// behaviour, which is why every existing caller compiles unchanged.
[[nodiscard]] std::optional<TextValueType>
declared_text_type(const ir::Component& component, std::span<const ir::RecordType> records,
                   std::string_view id);

// Static type inference for a text interpolation expression. `locals` supplies the loop item/index
// of a generated list-item builder; it is empty outside a template.
[[nodiscard]] std::optional<TextValueType> infer_text_type(const ir::Expr& expr,
                                                           const ir::Component& component,
                                                           std::span<const ir::RecordType> records,
                                                           const LocalBindings& locals = {});

// True when `expr` has a printable text form (interpolation or scalar formatting).
[[nodiscard]] bool printable_text_expression(const ir::Expr& expr, const ir::Component& component,
                                             std::span<const ir::RecordType> records,
                                             const LocalBindings& locals = {});
// Emits the value ops for one text expression. `hash_var` non-empty accumulates a value hash
// without allocation (the change-detection pass); empty with `append_target` non-empty appends
// formatted text to that string (the rebuild pass). Exactly one mode is active.
[[nodiscard]] bool emit_value_ops(CppWriter& writer, const ir::Expr& expr,
                                  const ir::Component& component,
                                  std::span<const ir::RecordType> records,
                                  const LocalBindings& locals, const std::string& hash_var,
                                  const std::string& append_target, std::string& error);

} // namespace tessera::compiler::codegen::internal
