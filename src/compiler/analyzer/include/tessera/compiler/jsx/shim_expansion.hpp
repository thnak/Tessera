// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"

// Shim packages (ADR-058 class B; docs/specs/router-shim-v1.md, docs/specs/store-i18n-shims-v1.md).
//
// A shim keeps a popular npm package's import specifier and a documented API subset and is
// implemented natively. This pass is the front-end half: it checks every import of a shim package
// against that package's coverage table and rewrites the covered forms into constructs the rules,
// lowering and code generation already accept, so no shim reaches the IR.
//
// It never emulates silently. A name outside the table, a covered name written in a form outside
// the table, and a covered name whose lowering has not landed are each a diagnostic that names the
// supported form (`TESSERA_SHIM_UNSUPPORTED`, `TESSERA_SHIM_FORM`, `TESSERA_SHIM_PENDING`).
namespace tessera::compiler::jsx {

// Checks and rewrites `module` in place; the import items of shim packages are removed, because
// the package has no source to resolve. Runs before the rules, from `expand_custom_hooks`.
void expand_shims(Module& module, DiagnosticSink& sink);

// True for a package specifier this pass owns (`react-router-dom`, `zustand`, ...).
[[nodiscard]] bool is_shim_package(std::string_view specifier) noexcept;

} // namespace tessera::compiler::jsx
