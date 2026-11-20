// SPDX-License-Identifier: MIT

// Selector specificity (stylesheet-v1 section 7.2). Split from selector.cpp (the parser) so each
// file stays under the source-size limit: specificity is a pure function of the parsed tree and
// shares nothing with the tokenizer walk.

#include "tessera/ui/style/selector.hpp"

namespace tessera::ui::style {

Specificity specificity(const CompoundSelector& selector) {
    Specificity result;
    if (!selector.tag.empty()) {
        ++result.types;
    }
    if (!selector.id.empty()) {
        ++result.ids;
    }
    result.classes += static_cast<std::uint32_t>(selector.classes.size());
    result.classes += static_cast<std::uint32_t>(selector.attributes.size());

    for (const PseudoClass& pseudo : selector.pseudos) {
        if (pseudo.pseudo_element) {
            ++result.types;
            continue;
        }
        if (pseudo.name == "where") {
            continue;
        }
        if ((pseudo.name == "is" || pseudo.name == "not" || pseudo.name == "has") &&
            pseudo.nested != nullptr) {
            result += specificity(*pseudo.nested);
            continue;
        }
        ++result.classes;
    }
    return result;
}

Specificity specificity(const ComplexSelector& selector) {
    Specificity result;
    for (const CompoundSelector& compound : selector.compounds) {
        result += specificity(compound);
    }
    return result;
}

Specificity specificity(const SelectorList& selectors) {
    Specificity result;
    for (const ComplexSelector& selector : selectors.selectors) {
        const Specificity current = specificity(selector);
        if (current > result) {
            result = current;
        }
    }
    return result;
}

} // namespace tessera::ui::style
