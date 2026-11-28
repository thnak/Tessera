// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/hook_expansion.hpp"
#include "tessera/compiler/jsx/hook_manifest.hpp"
#include "tessera/compiler/rules/component_analysis.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace tessera::compiler::rules {

struct AnalyzedModule; // the module pass's output; `check_module_contexts` writes into it

// Per-component reporting state: forwards to the module's sink and remembers whether this
// component reported an error, independent of the sink's diagnostic limit.
struct Context {
    const jsx::ModuleItem& item;
    ComponentAnalysis& out;
    jsx::DiagnosticSink& sink;
    // The built-in hook manifest: the authority on what a library hook accepts and returns.
    const jsx::HookManifest& manifest;
    // The module the component was parsed from. ADR-066 item 4's `useReducer` form is defined by a
    // module-scope helper and by the type aliases beside it, so the recognition pass reads them.
    const jsx::Module& module;
    // The custom-hook expansion's call-site map (spike #659, ADR-065; docs/specs/custom-hooks-v1.md
    // section 8), or null when the expansion did not run. A diagnostic whose span falls inside an
    // expanded region carries that region's call site as its related span, so the reader sees the
    // line they wrote rather than one inside a hook body.
    const jsx::CallSiteMap* call_sites = nullptr;
    // ADR-066 item 5 (issue #802): the module's `createContext` consts, filled by
    // `check_module_contexts` before the per-component passes run. `check_contexts` validates each
    // `useContext` argument against them.
    const std::vector<ContextInfo>* module_contexts = nullptr;
    // ADR-050 section 3.1 item 1 (issue #177): the project's `ui/services.json`, or null when the
    // project has none. `useService<T>()` resolves `T` against it and a service call is checked
    // against its methods.
    const services::ServicesManifest* services = nullptr;
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

// Adds one name to the component's symbol table. Shared with reducer.cpp, which declares the two
// names of a `useReducer` statement.
void add_symbol(Context& ctx, std::string name, SymbolKind kind, jsx::Span span,
                std::size_t statement);

// ADR-066 item 4: whether `expr` is a call of a `useReducer` dispatch, which a handler body may
// hold like a setter call. Defined in reducer.cpp, which owns the form.
[[nodiscard]] bool is_dispatch_call(const Context& ctx, const jsx::Expr& expr);

// Passes, in the order analyze_module runs them.
void check_hooks(Context& ctx);          // hooks.cpp: symbols and forms, then the placement checks
void check_hook_placement(Context& ctx); // hook_placement.cpp: nesting and declaration order
void classify_arrows(Context& ctx);      // arrows.cpp: roles, BIND_MISSING, effect returns
void check_effect_deps(Context& ctx);    // effect_deps.cpp: exact dependency match
// memo_deps.cpp: a memo/callback list is compared, not required to match exactly (issue #829).
void check_memo_deps(Context& ctx);
// reducer.cpp: the `useReducer` form, run after the hook forms so its symbols exist.
void check_reducers(Context& ctx);
// context.cpp: ADR-066 item 5 (issue #802). The module pass collects the `createContext` consts;
// the component pass validates each `useContext` against them, run after the hook forms so its
// symbol exists.
void check_module_contexts(const jsx::Module& module, AnalyzedModule& out,
                           jsx::DiagnosticSink& sink);
void check_contexts(Context& ctx);
// service.cpp: ADR-050 section 3.1 (issue #177). `check_services` records each `useService<T>()`
// and declares its symbol, run after the hook forms like `check_contexts`; `check_service_calls`
// then checks every use of a service symbol (method, arity, handler-only) once the symbols exist.
void check_services(Context& ctx);
void check_service_calls(Context& ctx);
// resource.cpp: ADR-067 (issue #656). `check_resource` is the form of one `useResource` statement
// and declares its symbol (called from the hook pass, so the order checks see it);
// `check_resource_services` resolves each recorded call against the `useService` bindings and the
// services manifest, run after `check_services`. `check_deferred_value` is the form of
// `useDeferredValue(x)`.
void check_resource(Context& ctx, std::size_t index, const jsx::Statement& statement,
                    const jsx::Expr& call);
void check_resource_services(Context& ctx);
void check_deferred_value(Context& ctx, std::size_t index, const jsx::Statement& statement,
                          const jsx::Expr& call, const jsx::HookEntry& hook);
// Whether `call` is `useResource(...)`; the service pass skips its first argument, which names a
// service method as a value.
[[nodiscard]] bool is_resource_call(const jsx::Expr& call);
// Whether `expr` is `svc.method(...)` on a service symbol of a known method, which a handler body
// may hold like a setter call. Defined in service.cpp, which owns the form.
[[nodiscard]] bool is_service_call(const Context& ctx, const jsx::Expr& expr);

} // namespace tessera::compiler::rules
