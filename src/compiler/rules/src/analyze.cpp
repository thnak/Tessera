// SPDX-License-Identifier: MIT

#include "tessera/compiler/rules/analyze.hpp"

#include "reporter.hpp"

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
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const jsx::Diagnostic& d) {
        return d.severity == jsx::Severity::Error;
    });
}

const ComponentAnalysis* AnalyzedModule::find_component(std::size_t item_index) const {
    const auto found =
        std::find_if(components.begin(), components.end(), [&](const ComponentAnalysis& c) {
            return c.item_index == item_index;
        });
    return found == components.end() ? nullptr : &*found;
}

AnalyzedModule analyze_module(const jsx::Module& module, const jsx::SourceMap& map,
                              const jsx::Limits& limits) {
    jsx::DiagnosticSink sink(map, limits);
    AnalyzedModule result;
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
        Context ctx{item, analysis, sink};
        check_hooks(ctx);
        classify_arrows(ctx);
        check_effect_deps(ctx);
        analysis.ok = !ctx.failed;
        result.components.push_back(std::move(analysis));
    }
    result.diagnostics = sink.take();
    return result;
}

} // namespace tessera::compiler::rules
