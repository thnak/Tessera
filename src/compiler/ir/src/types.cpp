// SPDX-License-Identifier: MIT

// The framework's predeclared payload records (docs/specs/ir-v1.md section 2.2, ADR-051).

#include "tessera/compiler/ir/types.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace tessera::compiler::ir {
namespace {

// Mirrors the `type_records()` table of `tessera_schema` (src/schema/src/types.cpp), which is the
// source of truth; `ir_record` asserts the two sets agree so they cannot drift. The IR names them
// only, because a payload record's member types are element-schema data.
constexpr std::array<std::string_view, 12> kPredeclaredRecords{{
    "PointerEvent",
    "ClickEvent",
    "KeyEvent",
    "FocusEvent",
    "TextChangeEvent",
    "TextSubmitEvent",
    "TextTarget",
    "TextSelection",
    "CompositionEvent",
    "ScrollEvent",
    "SizeChangeEvent",
    "ErrorInfo",
}};

} // namespace

std::span<const std::string_view> predeclared_record_names() {
    return kPredeclaredRecords;
}

bool is_predeclared_record(std::string_view name) {
    return std::find(kPredeclaredRecords.begin(), kPredeclaredRecords.end(), name) !=
           kPredeclaredRecords.end();
}

} // namespace tessera::compiler::ir
