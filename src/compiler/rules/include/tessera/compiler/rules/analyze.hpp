// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/hook_manifest.hpp"
#include "tessera/compiler/jsx/limits.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/rules/analyzed_module.hpp"

namespace tessera::compiler::rules {

// Tier 1 declaration-dependent checks of docs/design/jsx-ir-lowering.md section 3 that exist so
// far: hook placement, order and forms, arrow roles, `return` placement in effects, effect
// dependencies and un-annotated declared arrows. Items the parser marked invalid are skipped
// (ok == false, no diagnostics). `map` must be the SourceMap of the source the module was parsed
// from. All state is local to the call, so concurrent calls on separate modules are safe.
// The built-in hook manifest, parsed once. A caller that has one passes it; the default is the
// committed manifest, so a test that only exercises core hooks needs no plumbing. A build that
// cannot read the manifest gets an empty one, which makes every library hook an unknown hook rather
// than silently accepting it.
[[nodiscard]] const jsx::HookManifest& builtin_hook_manifest();

AnalyzedModule analyze_module(const jsx::Module& module, const jsx::SourceMap& map,
                              const jsx::Limits& limits = {},
                              const jsx::HookManifest* manifest = nullptr);

} // namespace tessera::compiler::rules
