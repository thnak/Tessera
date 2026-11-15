// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/types.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/lowering/expression_lowering.hpp"
#include "tessera/compiler/lowering/type_alias.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Internal seam of ADR-051 slice 3: the module record type table and the object-literal lowering
// that feeds an `ir::Record` (docs/specs/ir-v1.md sections 2.2 and 4.3). Kept apart from
// `node_lowering_internal.hpp` so the hook path (a `useState` initial) and the props path (a prop
// default) can both use it without depending on the node state.
namespace tessera::compiler::lowering::internal {

// One project file as the record-type collection sees it.
struct RecordTypeSource {
    std::string_view file;
    const jsx::Module* module = nullptr;
    const jsx::SourceMap* source_map = nullptr;
};

// The module's record type table (ir-v1 section 2.2): one entry per record declared anywhere in the
// project, sorted by name. `names` is the project's resolved type-name table, so an alias of an
// imported alias resolves to the one object body it names (issue #70). An alias that ultimately
// names a non-object type is not a record and is skipped; a duplicate record name is
// `TESSERA_IR_DUPLICATE_ID`, and a member with no IR form or a `children`/callback member is
// `TESSERA_IR_MALFORMED`. Both stop the build.
[[nodiscard]] std::expected<std::vector<ir::RecordType>, jsx::Diagnostic>
collect_record_types(std::span<const RecordTypeSource> files, const TypeNameTable& names);

// The record name `type` names, allowing one `optional<T>` wrapper (an optional record prop or a
// nullable record state), or nullopt when it is not a declared record.
[[nodiscard]] std::optional<std::string_view> record_name_of(std::span<const ir::RecordType> table,
                                                             std::string_view type);

// The declared record `name`, or null.
[[nodiscard]] const ir::RecordType* find_record(std::span<const ir::RecordType> table,
                                                std::string_view name);

// The single type inside any `list<>`/`optional<>` wrappers (`list<Size>` -> `Size`), or nullopt
// when `type` is not a list type. Used for the array-of-literals field form.
[[nodiscard]] std::optional<std::string_view> list_element_of(std::string_view type);

// What the record lowerer needs: where spans live and what to report into. Separate from
// `NodeState`/`HookState` so the two call sites share one lowerer.
struct RecordLoweringState {
    const jsx::SourceMap& source_map;
    std::string_view file;
    std::optional<jsx::Diagnostic> error; // the first error; later reports are ignored
};

// Lowers an object literal in a declared record's position to an `ir::Record` (section 4.3). Fields
// follow the record's member order, not the source's, because that is the generated aggregate's
// designated-initializer order and the validator's `TESSERA_IR_ORDER` rule.
//
// `TESSERA_IR_RECORD_VALUE`: the value is not an object literal, a required member is missing, a
// key is not a member, a key repeats. `TESSERA_IR_UNSUPPORTED`: a computed field value (a `ref`, a
// call, a binary expression, a shorthand identifier), an array where the member is not a list, a
// list of records, or a nested object whose member type is not a declared record. Those are the
// forms this slice does not accept, and each names the member.
[[nodiscard]] ir::ExprPtr lower_record_value(RecordLoweringState& state, const jsx::Expr& object,
                                             const ir::RecordType& record,
                                             std::span<const ir::RecordType> records);

} // namespace tessera::compiler::lowering::internal
