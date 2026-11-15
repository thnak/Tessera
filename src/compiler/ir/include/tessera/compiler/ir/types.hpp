// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/source_span.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::ir {

// docs/specs/ir-v1.md section 2.2: the module-level record type table (ADR-051). A record is a
// TypeScript object type with named, typed members (`type Size = { w: int; h: int }`); the first
// slice accepts it as a `useState` initial or a prop default.

// One member of a declared record. `optional` is the source `?`: a record value may omit it.
struct RecordMember {
    std::string name;
    std::string type; // an IR type of section 2: primitive, list<T>, optional<T> or a record name
    bool optional = false;
};

// One entry of `IrModule.types`. `id` is `type::<name>`; members keep declaration order, which is
// the aggregate-initialization order of the generated C++ (ADR-051 decision 5).
struct RecordType {
    std::string id;
    std::string name;
    std::vector<RecordMember> members;
    SourceSpan source;
};

// The framework's payload records (the typed event objects of
// docs/specs/jsx-intrinsics-v1/common-and-events.md section 2: `TextChangeEvent`, `KeyEvent`, ...).
// They are valid IR type names for a prop or a handler parameter, but they are NOT listed in
// `types`: their identity and their values belong to the framework, not to the project. The IR
// layer keeps only their names, because its member types live in the element schema and
// `tessera_ir` links no project library; a dotted `ref` into a payload record is therefore not
// member-checked here (docs/specs/ir-v1.md section 9.1, "deliberately not covered").
std::span<const std::string_view> predeclared_record_names();

// True when `name` is one of `predeclared_record_names()`.
bool is_predeclared_record(std::string_view name);

} // namespace tessera::compiler::ir
