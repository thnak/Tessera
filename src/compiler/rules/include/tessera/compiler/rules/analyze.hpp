// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/hook_expansion.hpp"
#include "tessera/compiler/jsx/hook_manifest.hpp"
#include "tessera/compiler/jsx/limits.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/rules/analyzed_module.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

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

// `services` is the project's `ui/services.json` (docs/specs/services-manifest-v1.md), or null when
// the project has none: `useService<T>()` then has no interface to resolve `T` against and is
// rejected, which is what makes an undeclared service a compile error (ADR-050 section 3.1 item 2).
// The manifest must outlive the returned analysis, which points into it.
//
// `call_sites` is the custom-hook expansion's map (spike #659, ADR-065;
// docs/specs/custom-hooks-v1.md section 8), or null when the expansion did not run. When it is
// given, a diagnostic whose span falls inside an expanded region carries that region's call site as
// its related span, so a diagnostic raised inside a hook body points at the line the author wrote.
AnalyzedModule analyze_module(const jsx::Module& module, const jsx::SourceMap& map,
                              const jsx::Limits& limits = {},
                              const jsx::HookManifest* manifest = nullptr,
                              const jsx::CallSiteMap* call_sites = nullptr,
                              const services::ServicesManifest* services = nullptr);

} // namespace tessera::compiler::rules
