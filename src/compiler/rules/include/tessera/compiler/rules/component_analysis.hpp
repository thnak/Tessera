// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/rules/arrow_role.hpp"
#include "tessera/compiler/rules/effect_info.hpp"
#include "tessera/compiler/rules/symbol.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace tessera::compiler::rules {

// The analysis of one component. Points into the Module it was built from, which must outlive it.
struct ComponentAnalysis {
    const jsx::ModuleItem* item = nullptr;
    std::size_t item_index = 0; // index into Module::items
    // False when the parser marked the item invalid (nothing analysed) or analysis reported an
    // error. Lowering emits no IR for a component with ok == false.
    bool ok = true;
    std::vector<Symbol> symbols;   // declaration order: props first, then body statements
    std::vector<EffectInfo> effects; // body order
    std::vector<ArrowInfo> arrows; // source order of first occurrence

    // First declaration of `name`, or null.
    [[nodiscard]] const Symbol* find(std::string_view name) const;
    // Role of `arrow`, or null when it is not an arrow of this component.
    [[nodiscard]] const ArrowInfo* find_arrow(const jsx::Expr* arrow) const;
};

} // namespace tessera::compiler::rules
