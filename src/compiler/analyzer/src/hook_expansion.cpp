// SPDX-License-Identifier: MIT

// The custom-hook expansion pass (spike #659, ADR-065; docs/specs/custom-hooks-v1.md section 4).
//
// For each call site, in source order: resolve the callee, instantiate a copy of the hook's body,
// rename every name the hook declares to `<hook>_<n>__<name>`, bind the parameters, splice the
// statements at the call position and substitute the returned values. The result is exactly what
// the author could have written by hand, so the rules, lowering and code generation see an
// ordinary component and need no change.
//
// The pass is off by default (`FrontEndOptions::custom_hooks`): with it off this file does nothing
// and the front end keeps the `TESSERA_REACT_CUSTOM_HOOK` rejection.

#include "hook_expansion_expander.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::jsx {

using internal::Substitutions;

const HookDeclaration* Expander::resolve(std::string_view name) const {
    for (const HookDeclaration& declaration : declarations_) {
        if (declaration.name == name) {
            return &declaration;
        }
    }
    return nullptr;
}

void Expander::report(std::string_view code, std::string message, Span span,
                      std::string suggestion) {
    failed_ = true;
    sink_.report(code, Severity::Error, std::move(message), span, std::move(suggestion),
                 related_for());
}

// Spec section 8: a diagnostic whose primary span is inside a hook carries the call site as its
// related span, with the text "expanded here, from <hook>_<n>". The expansion's own diagnostics are
// reported at the call site already, so they need no related span; this is for the rules.
//
// The call site is the one the author wrote, which is the outermost expansion on the stack: a
// diagnostic raised while expanding a nested hook is inside the outer hook's body, and the outer
// call is the line the reader can act on. With no expansion in progress the diagnostic is the
// component's own, so it needs no related span.
std::optional<RelatedSpan> Expander::related_for() const {
    if (stack_.empty()) {
        return std::nullopt;
    }
    const Expansion& outermost = stack_.front();
    return RelatedSpan{map_.resolve(outermost.call_span), "expanded here, from " + outermost.site};
}

Substitutions Expander::expand_component(ModuleItem& component) {
    Substitutions scope = expand_body(component.body, 0);
    if (failed_ || scope.empty() || !component.returned) {
        return scope;
    }
    std::vector<Span> whole_value_uses;
    internal::substitute(*component.returned, scope, whole_value_uses);
    for (const Span& span : whole_value_uses) {
        report("TESSERA_REACT_HOOK_RETURN",
               "a whole-value binding of an object return has no run-time record", span,
               "read a field (`p.page`) or bind the fields with an object pattern");
    }
    return scope;
}

Substitutions Expander::expand_body(std::vector<Statement>& body, std::size_t depth) {
    Substitutions scope;
    // A statement may splice in several, so the loop re-reads the size: the spliced statements are
    // themselves expanded (spec section 4 step 7).
    for (std::size_t index = 0; index < body.size(); ++index) {
        if (!body[index].init) {
            continue;
        }
        // The scope so far applies to this statement before it is expanded, so an argument that
        // reads an earlier call site's output is already the hook's own source.
        std::vector<Span> whole_value_uses;
        internal::substitute(body[index].init, scope, whole_value_uses);
        for (const Span& span : whole_value_uses) {
            report("TESSERA_REACT_HOOK_RETURN",
                   "a whole-value binding of an object return has no run-time record", span,
                   "read a field (`p.page`) or bind the fields with an object pattern");
        }
        const std::string_view name = callee_name(*body[index].init);
        if (name.empty() || resolve(name) == nullptr) {
            continue;
        }
        const std::size_t inserted = expand_call(body, index, depth, scope);
        if (failed_) {
            return scope;
        }
        index += inserted == 0 ? 0 : inserted - 1;
    }
    return scope;
}

std::vector<HookDeclaration> collect_hook_declarations(const Module& module, DiagnosticSink& sink) {
    std::vector<HookDeclaration> declarations;
    for (const ModuleItem& item : module.items) {
        if (item.kind != ItemKind::Hook) {
            continue;
        }
        const bool duplicate =
            std::any_of(declarations.begin(), declarations.end(),
                        [&](const HookDeclaration& known) { return known.name == item.name; });
        if (duplicate) {
            sink.error("TESSERA_REACT_HOOK_FORM",
                       "hook `" + excerpt(item.name) + "` is declared twice in this file",
                       item.name_span, "rename one of them");
            continue;
        }
        declarations.push_back(HookDeclaration{item.name, &item});
    }
    return declarations;
}

const ExpandedRegion* CallSiteMap::find(Span span) const {
    const ExpandedRegion* best = nullptr;
    for (const ExpandedRegion& region : regions) {
        if (span.offset < region.span.offset || span.end() > region.span.end()) {
            continue;
        }
        if (best == nullptr || region.span.length < best->span.length) {
            best = &region;
        }
    }
    return best;
}

ExpansionMetrics expand_custom_hooks(Module& module, const SourceMap& map,
                                     const FrontEndOptions& options, DiagnosticSink& sink) {
    (void)map;
    ExpansionMetrics metrics;
    if (!options.custom_hooks) {
        return metrics;
    }
    const std::vector<HookDeclaration> declarations = collect_hook_declarations(module, sink);
    if (declarations.empty()) {
        return metrics;
    }
    Expander expander(declarations, map, sink, metrics);
    for (ModuleItem& item : module.items) {
        if (item.kind != ItemKind::Component) {
            continue;
        }
        const std::size_t before = item.body.size();
        expander.expand_component(item);
        // The expansion can SHRINK the body: a hook whose body is only a `return` contributes no
        // statements, so the call statement is replaced by nothing. Subtracting the sizes would
        // underflow to SIZE_MAX and report a size error for a hook that added no code at all.
        const std::size_t after = item.body.size();
        const std::size_t added = after > before ? after - before : 0;
        metrics.added_statements.push_back(added);
        if (added > kMaxStatements) {
            sink.error("TESSERA_REACT_HOOK_SIZE",
                       "expanding the hooks of `" + excerpt(item.name) + "` adds " +
                           std::to_string(added) + " statements, more than " +
                           std::to_string(kMaxStatements),
                       item.name_span, "split the component or inline the hook");
        } else if (added > kWarnStatements) {
            sink.report("TESSERA_REACT_HOOK_SIZE", Severity::Warning,
                        "expanding the hooks of `" + excerpt(item.name) + "` adds " +
                            std::to_string(added) + " statements",
                        item.name_span, "split the component or inline the hook");
        }
        if (expander.failed()) {
            return metrics;
        }
    }
    return metrics;
}

} // namespace tessera::compiler::jsx
