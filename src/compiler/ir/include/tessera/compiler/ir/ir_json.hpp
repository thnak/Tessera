// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/json_value.hpp"
#include "tessera/compiler/ir/module.hpp"

#include <string>

namespace tessera::compiler::ir {

// IR -> JSON (docs/specs/ir-v1.md). The text is a pure function of the IR:
//   - keys are in byte order, two-space indent, LF, one trailing newline (see `to_text`);
//   - `components`, `bindings` and `stylesheets` of the module are always written, even when
//     empty; every other empty or absent optional field is omitted;
//   - an expression in a value position (props, style, key, `default`, `initial`) is written as
//     the bare JSON value when it is a literal and as its object otherwise; everywhere else an
//     expression is always its object.
// A malformed IR (missing operand, null pointer) is written with `null` in the gap rather than
// failing; the validator is what reports it.
JsonValue to_json(const IrModule& module);
JsonValue to_json(const Component& component);
JsonValue to_json(const Binding& binding);
JsonValue to_json(const Node& node);
JsonValue to_json(const SourceSpan& span);

// Expression, always as its object (`{"kind": "literal", "value": 1}`); null for nullptr.
JsonValue expr_to_json(const Expr* expr);
// Expression in a value position (bare literal).
JsonValue value_to_json(const Expr* expr);

std::string write_ir_json(const IrModule& module);

} // namespace tessera::compiler::ir
