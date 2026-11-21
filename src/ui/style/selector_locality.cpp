// SPDX-License-Identifier: MIT

#include "selector_locality.hpp"

#include <string_view>

namespace tessera::ui::style {

namespace {

bool structural_pseudo(std::string_view name) {
    for (const std::string_view structural :
         {"has", "nth-child", "nth-last-child", "nth-of-type", "nth-last-of-type", "first-child",
          "last-child", "only-child", "first-of-type", "last-of-type", "only-of-type", "empty"}) {
        if (name == structural) {
            return true;
        }
    }
    return false;
}

} // namespace

bool selector_is_subtree_local(const ComplexSelector& complex) {
    for (const Combinator combinator : complex.combinators) {
        if (combinator == Combinator::AdjacentSibling || combinator == Combinator::GeneralSibling) {
            return false;
        }
    }
    for (const CompoundSelector& compound : complex.compounds) {
        for (const PseudoClass& pseudo : compound.pseudos) {
            if (structural_pseudo(pseudo.name) ||
                (pseudo.nested != nullptr && !selector_list_is_subtree_local(*pseudo.nested))) {
                return false;
            }
        }
    }
    return true;
}

bool selector_list_is_subtree_local(const SelectorList& list) {
    for (const ComplexSelector& complex : list.selectors) {
        if (!selector_is_subtree_local(complex)) {
            return false;
        }
    }
    return true;
}

} // namespace tessera::ui::style
