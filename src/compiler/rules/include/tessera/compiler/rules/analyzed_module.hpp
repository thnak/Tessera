// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/rules/component_analysis.hpp"

#include <vector>

namespace tessera::compiler::rules {

// Result of analyze_module: one ComponentAnalysis per component item, in source order, and every
// diagnostic in report order (component order, then statement order within a component).
struct AnalyzedModule {
    std::vector<ComponentAnalysis> components;
    // ADR-066 item 5 (issue #802): the module-scope `createContext` consts, in source order. A
    // rejected one is a diagnostic and contributes no entry. Contexts are module facts, not
    // component facts: the provider tree walk and every consumer read them.
    std::vector<ContextInfo> contexts;
    std::vector<jsx::Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const;
    // Analysis of the component at Module::items[item_index], or null for other items.
    [[nodiscard]] const ComponentAnalysis* find_component(std::size_t item_index) const;
    // The context const `name` declares, or null.
    [[nodiscard]] const ContextInfo* find_context(std::string_view name) const;
};

} // namespace tessera::compiler::rules
