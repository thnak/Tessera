// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <cstdint>
#include <string>

namespace ir_test {

namespace ir = tessera::compiler::ir;

ir::SourceSpan span(std::uint32_t offset, std::uint32_t length, std::uint32_t line,
                    std::uint32_t column, std::uint32_t end_line, std::uint32_t end_column);

// The Counter example of docs/specs/ir-v1.md section 13 (golden tests/golden/ir/counter.json).
ir::IrModule make_counter_module();
// Exercises every optional field of the writer (golden tests/golden/ir/rich.json).
ir::IrModule make_rich_module();
// A module that passes validation and uses every kind of reference the validator resolves:
// two components (List, Row), a Component node with a loop, handlers, an effect with a cleanup,
// a native-handler binding with a preview, an ErrorBoundary and a ref.
ir::IrModule make_valid_module();

} // namespace ir_test
