// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/lowering/type_alias.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Internal seam of cross-file props-alias resolution (issue #70, slice 4). Module lowering resolves
// each file's module paths first and leaves the names here; this layer then follows imports,
// re-exports and aliases to the one declaration that owns a type name. Not installed.
namespace tessera::compiler::lowering::internal {

inline constexpr std::size_t kNoProjectFile = static_cast<std::size_t>(-1);

// One module reference that still needs name resolution: a named import or a type re-export
// pointing at a project file, or a local re-export (`target == kNoProjectFile`).
struct ProjectModuleRef {
    const jsx::ModuleItem* item = nullptr;
    std::size_t target = kNoProjectFile;
};

// One project file as import resolution leaves it: module paths resolved, names not yet.
struct ProjectFileScope {
    std::string_view file;
    const jsx::Module* module = nullptr;
    const jsx::SourceMap* source_map = nullptr;
    std::span<const ProjectModuleRef> imports;
    std::span<const ProjectModuleRef> re_exports;
};

// The built table plus the storage the table's spans point into. Keep it alive while lowering.
struct ProjectTypeNames {
    std::vector<TypeNameEntry> entries;
    std::vector<TypeFileMap> files;
    TypeNameTable table;
    std::optional<jsx::Diagnostic> error; // the first resolution error, if any
};

// Resolves every imported and re-exported type name of the project and builds the visible-name
// table. A name that no file exports is `TESSERA_IR_UNRESOLVED` (or `TESSERA_IR_TYPE_UNRESOLVED`
// for a type-only import), and a cycle through imports/re-exports is `TESSERA_IR_TYPE_CYCLE`; both
// stop the build with no table (the caller must not lower without one).
[[nodiscard]] ProjectTypeNames build_type_names(std::span<const ProjectFileScope> files);

} // namespace tessera::compiler::lowering::internal
