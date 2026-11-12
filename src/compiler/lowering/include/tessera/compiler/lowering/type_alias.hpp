// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/source_map.hpp"

#include <cstdint>
#include <span>
#include <string_view>

// Slice 4 (cross-file): the type names visible in one project file's scope, and the type-alias
// declarations they resolve to (issue #70). `lower_project` builds the table once from the files'
// imports and re-exports; the props-record lowering only looks names up, so an imported alias and
// a same-file alias produce the same `ir::Prop` records
// (docs/design/jsx-ir-lowering.md "Types (props records/defaults done)").
namespace tessera::compiler::lowering {

enum class TypeNameKind : std::uint8_t {
    Alias, // a `type` declaration, possibly in another project file
    Value, // a component or constant: a name that cannot be used as a props type
};

// One name visible in one project file's scope.
struct TypeNameEntry {
    std::string_view file; // the file whose scope the name belongs to
    std::string_view name; // the name as written in that scope
    TypeNameKind kind = TypeNameKind::Alias;
    std::string_view alias_file; // the file declaring `alias` (Alias only)
    const jsx::ModuleItem* alias = nullptr;
};

// One project file and its source map, so a diagnostic can resolve a span inside a file other than
// the component's own file.
struct TypeFileMap {
    std::string_view file;
    const jsx::SourceMap* map = nullptr;
};

// The type names of every file of one lowering call. `entries` is sorted by (file, name) and
// `files` by file, so lookups and diagnostics are deterministic.
struct TypeNameTable {
    std::span<const TypeNameEntry> entries;
    std::span<const TypeFileMap> files;

    // The declaration of `name` visible in `file`, or null when the file does not name it.
    [[nodiscard]] const TypeNameEntry* find(std::string_view file, std::string_view name) const;

    // The source map of `file`, or null when the table does not cover it.
    [[nodiscard]] const jsx::SourceMap* map_of(std::string_view file) const;
};

} // namespace tessera::compiler::lowering
