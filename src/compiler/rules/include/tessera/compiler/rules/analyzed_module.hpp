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
    std::vector<jsx::Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const;
    // Analysis of the component at Module::items[item_index], or null for other items.
    [[nodiscard]] const ComponentAnalysis* find_component(std::size_t item_index) const;
};

} // namespace tessera::compiler::rules
