// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/rules/component_analysis.hpp"

#include <string>
#include <string_view>

namespace tessera::compiler::rules {

// Per-component reporting state: forwards to the module's sink and remembers whether this
// component reported an error, independent of the sink's diagnostic limit.
struct Context {
    const jsx::ModuleItem& item;
    ComponentAnalysis& out;
    jsx::DiagnosticSink& sink;
    bool failed = false;

    void error(std::string_view code, std::string message, jsx::Span span,
               std::string suggestion = {}) {
        failed = true;
        sink.error(code, std::move(message), span, std::move(suggestion));
    }
};

// A call of a hook that is not one of the four core hooks: UNSUPPORTED_API for the React APIs the
// subset names (spec section 12), UNKNOWN_HOOK for anything else starting with `use`.
void report_unknown_hook(Context& ctx, std::string_view name, jsx::Span span);

// Passes, in the order analyze_module runs them.
void check_hooks(Context& ctx);       // hooks.cpp: symbols and forms, then the placement checks
void check_hook_placement(Context& ctx); // hook_placement.cpp: nesting and declaration order
void classify_arrows(Context& ctx);   // arrows.cpp: roles, BIND_MISSING, effect returns
void check_effect_deps(Context& ctx); // effect_deps.cpp: exact dependency match

} // namespace tessera::compiler::rules
