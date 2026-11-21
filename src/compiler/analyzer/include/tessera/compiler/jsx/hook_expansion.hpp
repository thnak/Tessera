// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/front_end_options.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/source_map.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Custom hook expansion (spike #659, ADR-065; docs/specs/custom-hooks-v1.md section 4).
//
// The pass rewrites a parsed module so that every call of a project-declared `use...` function is
// replaced by the statements of that hook, renamed per call site. The rules, lowering and code
// generation then see an ordinary component and need no change: a custom hook has no IR form, no
// generated class and no run-time cost.
//
// It is a parse-tree-to-parse-tree pass and runs before the rules. It is off by default: with
// `FrontEndOptions::custom_hooks` off, `expand_custom_hooks` does nothing and the front end keeps
// the `TESSERA_REACT_CUSTOM_HOOK` rejection, so the default pipeline is unchanged.
namespace tessera::compiler::jsx {

// One hook declaration of the module, resolved by name. `item` points into the module, so the
// declaration outlives the expansion.
struct HookDeclaration {
    std::string name;
    const ModuleItem* item = nullptr;
};

// The hook declarations of `module`, in source order. A duplicate name is `TESSERA_REACT_HOOK_FORM`
// and the first declaration wins, so a call site always resolves to one body.
[[nodiscard]] std::vector<HookDeclaration> collect_hook_declarations(const Module& module,
                                                                     DiagnosticSink& sink);

// One region of a component that came from a hook: the spliced statements' span (in the hook's own
// file) and the call site that produced it. The rules use it to attach the related span of spec
// section 8 to a diagnostic whose primary span is inside a hook.
struct ExpandedRegion {
    Span span;        // the spliced statements, in the hook's file
    Span call_span;   // the call site, in the component's file
    std::string hook; // `<hook>_<n>`, the name the expansion gave this call site
};

struct CallSiteMap {
    std::vector<ExpandedRegion> regions;

    // The innermost region containing `span`, or null. Innermost wins so a nested hook's diagnostic
    // names the call site of the nested call rather than the outer one.
    //
    // The spliced statements keep the position they had in the hook's own file (spec section 8), so
    // two calls of one hook produce two regions with the same `span`. A span can therefore name
    // only the first of them; a diagnostic that must distinguish the calls has to carry the call
    // site itself, which is what the expansion's own diagnostics do.
    [[nodiscard]] const ExpandedRegion* find(Span span) const;
};

// What one expansion did, for the size limit of spec section 7 and for the spike's measurements.
struct ExpansionMetrics {
    // Statements the expansion added, per component, in component order.
    std::vector<std::size_t> added_statements;
    // Every `bind::` id the expansion produced, in source order: the developer-class member names
    // of spec section 4 ("Native handlers").
    std::vector<std::string> bind_ids;
    CallSiteMap call_sites;
};

// Expands every custom hook call of `module` in place. `sink` supplies the source map the
// diagnostics are resolved through and receives them in report order. Returns what the pass did;
// a module whose diagnostics contain an error must not be lowered.
[[nodiscard]] ExpansionMetrics expand_custom_hooks(Module& module, const SourceMap& map,
                                                   const FrontEndOptions& options,
                                                   DiagnosticSink& sink);

} // namespace tessera::compiler::jsx
