// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/lowering/hook_lowering.hpp"
#include "tessera/compiler/rules/analyzed_module.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Slice 7 of docs/design/jsx-ir-lowering.md: lower the returned JSX element of one analysed
// component to the IR node tree of docs/specs/ir-v1.md section 3, on top of the hook and binding
// data of slice 6. Component references (slice 9), conditionals and loops (slice 8), the type table
// (slice 4), custom elements (slice 11) and lowercase HTML aliases (slice 13) are rejected with
// TESSERA_IR_* diagnostics.
namespace tessera::compiler::lowering {

// One fully lowered component: the IR component plus the bindings that belong to the module rather
// than to the component (docs/specs/ir-v1.md sections 1 and 6).
struct LoweredComponent {
    ir::Component component;
    std::vector<ir::Binding> bindings; // sorted by id, from slice 6
};

using NodeLoweringErrors = std::vector<jsx::Diagnostic>;
using LoweredComponentResult = std::expected<LoweredComponent, NodeLoweringErrors>;

// Lowers hooks (slice 6) and then the returned element. `analysis` and `component` must match and
// the analysis must have succeeded, as for `lower_hooks`. Node ids are `node::N` in source
// pre-order from 1 (decision 2); `componentId` and `uiId` are left empty for slice 10, and
// `automationId` is filled from the attribute of the same name when present. Handler records for
// arrows written directly in JSX continue the `handler::N` numbering of the declared handlers, and
// `Ref::target` is filled for every ref attribute.
//
// A failing component yields diagnostics and no component data at all (first error only).
[[nodiscard]] LoweredComponentResult lower_component(const HookLoweringContext& context);

// One parsed and analysed file of the project (slice 9; file reading stays with the caller,
// decision 13). `file` is the project-relative POSIX path without a leading "./".
struct ModuleSource {
    std::string_view file;
    const jsx::Module* module = nullptr;
    const rules::AnalyzedModule* analysis = nullptr;
    const jsx::SourceMap* source_map = nullptr;
};

// All components of a project plus its module-level data. `components` are sorted by source file
// and then by name (ir-v1 section 11) and `bindings` by id; `componentId`/`uiId` stay empty for
// slice 10.
struct LoweredModule {
    std::string name;
    std::vector<ir::Component> components;
    std::vector<ir::Binding> bindings;
    std::vector<ir::StylesheetRef> stylesheets; // sorted by path, one entry per file
};

using ModuleLoweringResult = std::expected<LoweredModule, NodeLoweringErrors>;

// Lowers one file with no cross-file imports; a relative import is an error here (use
// `lower_project`). Component references resolve to components declared in the same file. Mutual
// recursion is supported because references are not inlined (a `Component` node names its target).
[[nodiscard]] ModuleLoweringResult lower_module(const ModuleSource& source,
                                                std::string_view module_name);

// One resolved package root (ADR-014 project config): a bare specifier imports files under `root`,
// so `from "tessera-ui/Button"` resolves to `<root>/Button` and a bare `from "tessera-ui"` to
// `<root>/index`. `root` is a project-relative POSIX path without a leading `./`, matching the
// `ModuleSource::file` form.
struct PackageRoot {
    std::string_view name;
    std::string_view root;
};

// Joins `base` and a relative import source and collapses `.`/`..` segments, producing the same
// project-relative POSIX form `ModuleSource::file` uses (no leading `./`, no extension handling).
// Shared with the driver so import discovery and lowering cannot disagree about a path.
[[nodiscard]] std::string join_project_path(std::string_view base, std::string_view relative);

// One stylesheet the caller has read for a side-effect `import "./x.css"` (issue #80). `path` is
// the project-relative POSIX path in the `ModuleSource::file` form; `text` is the file content,
// hashed into `ir::StylesheetRef::hash`. A CSS import that names no entry (missing or unreadable
// file) is `TESSERA_IR_STYLESHEET_MISSING` at the import span.
struct StylesheetFile {
    std::string_view path;
    std::string_view text;
};

// Lowers every file of a project. Relative imports (`./X`, `../X`) are resolved against `files`,
// each imported name must be a component, type alias or constant of the target (directly or through
// its type re-exports), and an import cycle is an error. A component reference resolves to the
// local component of that name first, then to an import; an unknown tag is a
// `TESSERA_IR_UNSUPPORTED` naming it. Type-only imports (`import type`), `as` renames and `export
// type { ... }` re-exports resolve for a component's props type exactly as a same-file alias does
// (issue #70).
[[nodiscard]] ModuleLoweringResult lower_project(std::span<const ModuleSource> files,
                                                 std::string_view module_name);

// As above, with the project's package roots: a bare specifier (`from "tessera-ui"`,
// `from "tessera-ui/Button"`) resolves inside the matching root and then follows the ordinary
// cross-file rules. `"tessera"` (framework hooks/intrinsics/library components) and `"react"`
// stay built-in and are never package imports. An unknown package is `TESSERA_IR_UNSUPPORTED`; a
// known package without the named file is `TESSERA_IR_UNRESOLVED`.
[[nodiscard]] ModuleLoweringResult lower_project(std::span<const ModuleSource> files,
                                                 std::string_view module_name,
                                                 std::span<const PackageRoot> packages);

// As above, with the stylesheets the project's CSS imports resolve against.
[[nodiscard]] ModuleLoweringResult lower_project(std::span<const ModuleSource> files,
                                                 std::string_view module_name,
                                                 std::span<const PackageRoot> packages,
                                                 std::span<const StylesheetFile> stylesheets);

} // namespace tessera::compiler::lowering
