// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/hook_expansion.hpp"
#include "tessera/compiler/jsx/hook_manifest.hpp"
#include "tessera/compiler/rules/component_analysis.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace tessera::compiler::rules {

// Per-component reporting state: forwards to the module's sink and remembers whether this
// component reported an error, independent of the sink's diagnostic limit.
struct Context {
    const jsx::ModuleItem& item;
    ComponentAnalysis& out;
    jsx::DiagnosticSink& sink;
    // The built-in hook manifest: the authority on what a library hook accepts and returns.
    const jsx::HookManifest& manifest;
    // The custom-hook expansion's call-site map (spike #659, ADR-065; docs/specs/custom-hooks-v1.md
    // section 8), or null when the expansion did not run. A diagnostic whose span falls inside an
    // expanded region carries that region's call site as its related span, so the reader sees the
    // line they wrote rather than one inside a hook body.
    const jsx::CallSiteMap* call_sites = nullptr;
    bool failed = false;

    // The related span for a diagnostic at `span`: the call site of the expansion that produced it,
    // or null when the span is the component's own source.
    [[nodiscard]] std::optional<jsx::RelatedSpan> related_for(jsx::Span span) const {
        if (call_sites == nullptr) {
            return std::nullopt;
        }
        const jsx::ExpandedRegion* region = call_sites->find(span);
        if (region == nullptr) {
            return std::nullopt;
        }
        return jsx::RelatedSpan{sink.map().resolve(region->call_span),
                                "expanded here, from " + region->hook};
    }

    void error(std::string_view code, std::string message, jsx::Span span,
               std::string suggestion = {}) {
        failed = true;
        sink.report(code, jsx::Severity::Error, std::move(message), span, std::move(suggestion),
                    related_for(span));
    }

    // A warning does not fail the component, but it carries the same related span: a warning inside
    // a hook body is as confusing without the call site as an error is.
    void warning(std::string_view code, std::string message, jsx::Span span,
                 std::string suggestion = {}) {
        sink.report(code, jsx::Severity::Warning, std::move(message), span, std::move(suggestion),
                    related_for(span));
    }
};

// A call of a hook that is not one of the four core hooks: UNSUPPORTED_API for the React APIs the
// subset names (spec section 12), UNKNOWN_HOOK for anything else starting with `use`.
void report_unknown_hook(Context& ctx, std::string_view name, jsx::Span span);

// Passes, in the order analyze_module runs them.
void check_hooks(Context& ctx);          // hooks.cpp: symbols and forms, then the placement checks
void check_hook_placement(Context& ctx); // hook_placement.cpp: nesting and declaration order
void classify_arrows(Context& ctx);      // arrows.cpp: roles, BIND_MISSING, effect returns
void check_effect_deps(Context& ctx);    // effect_deps.cpp: exact dependency match

} // namespace tessera::compiler::rules
