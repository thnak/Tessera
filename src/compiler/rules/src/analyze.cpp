// SPDX-License-Identifier: MIT

#include "reporter.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <algorithm>

namespace tessera::compiler::rules {

const Symbol* ComponentAnalysis::find(std::string_view name) const {
    const auto found = std::find_if(symbols.begin(), symbols.end(),
                                    [&](const Symbol& symbol) { return symbol.name == name; });
    return found == symbols.end() ? nullptr : &*found;
}

const ArrowInfo* ComponentAnalysis::find_arrow(const jsx::Expr* arrow) const {
    const auto found = std::find_if(arrows.begin(), arrows.end(),
                                    [&](const ArrowInfo& info) { return info.arrow == arrow; });
    return found == arrows.end() ? nullptr : &*found;
}

bool AnalyzedModule::has_errors() const {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const jsx::Diagnostic& d) { return d.severity == jsx::Severity::Error; });
}

const ComponentAnalysis* AnalyzedModule::find_component(std::size_t item_index) const {
    const auto found =
        std::find_if(components.begin(), components.end(),
                     [&](const ComponentAnalysis& c) { return c.item_index == item_index; });
    return found == components.end() ? nullptr : &*found;
}

const ContextInfo* AnalyzedModule::find_context(std::string_view name) const {
    const auto found = std::find_if(contexts.begin(), contexts.end(),
                                    [&](const ContextInfo& info) { return info.name == name; });
    return found == contexts.end() ? nullptr : &*found;
}

const jsx::HookManifest& builtin_hook_manifest() {
    // Parsed once, on first use. The manifest is embedded at build time, so this is a pure function
    // of the build; a failure yields an empty manifest rather than a half-read one.
    static const jsx::HookManifest manifest = [] {
        const std::string_view text =
#include "hooks_manifest_text.inc"
            ;
        const std::optional<jsx::HookManifest> parsed = jsx::parse_hook_manifest(text);
        return parsed.value_or(jsx::HookManifest{});
    }();
    return manifest;
}

AnalyzedModule analyze_module(const jsx::Module& module, const jsx::SourceMap& map,
                              const jsx::Limits& limits, const jsx::HookManifest* manifest,
                              const jsx::CallSiteMap* call_sites,
                              const services::ServicesManifest* services) {
    const jsx::HookManifest& hooks = manifest != nullptr ? *manifest : builtin_hook_manifest();
    jsx::DiagnosticSink sink(map, limits);
    AnalyzedModule result;
    // ADR-066 item 5 (issue #802) first: the module's `createContext` consts, so every
    // component's `useContext` check can name them.
    check_module_contexts(module, result, sink);
    const std::vector<ContextInfo>& contexts = result.contexts;
    for (std::size_t index = 0; index < module.items.size(); ++index) {
        const jsx::ModuleItem& item = module.items[index];
        if (item.kind != jsx::ItemKind::Component) {
            continue;
        }
        ComponentAnalysis analysis;
        analysis.item = &item;
        analysis.item_index = index;
        if (!item.valid) {
            analysis.ok = false;
            result.components.push_back(std::move(analysis));
            continue;
        }
        Context ctx{item, analysis, sink, hooks, module, call_sites};
        ctx.module_contexts = &contexts;
        ctx.services = services;
        check_hooks(ctx);
        check_contexts(ctx);
        check_services(ctx);
        classify_arrows(ctx);
        check_service_calls(ctx);
        check_effect_deps(ctx);
        check_memo_deps(ctx);
        analysis.ok = !ctx.failed;
        result.components.push_back(std::move(analysis));
    }
    result.diagnostics = sink.take();
    return result;
}

} // namespace tessera::compiler::rules
