// SPDX-License-Identifier: MIT

// The engine's selector-support answer (stylesheet-v1 section 6.4): whether a selector list parses
// and every pseudo-class/pseudo-element in it has implemented matching semantics. `@supports
// selector(...)` evaluates with this. Split from selector.cpp (the parser) for the file limits.

#include "tessera/ui/style/selector.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::ui::style {
namespace {

bool is_supported_pseudo_class(std::string_view name) {
    return name == "hover" || name == "active" || name == "focus" || name == "focus-visible" ||
           name == "disabled" || name == "enabled" || name == "checked" || name == "required" ||
           name == "optional" || name == "visited" || name == "root" || name == "empty" ||
           name == "first-child" || name == "last-child" || name == "only-child" ||
           name == "first-of-type" || name == "last-of-type" || name == "nth-child" ||
           name == "nth-last-child" || name == "not" || name == "is" || name == "where";
}

// `::part()` is the only pseudo-element whose matching is implemented (V1); the parse-only
// pseudo-elements (`::before`, `::after`, `::placeholder`, `::selection`) never match a scene node,
// so `@supports selector(::before)` answers false.
bool is_supported_pseudo_element(std::string_view name) {
    return name == "part";
}

bool list_supported(const SelectorList& list);

bool compound_supported(const CompoundSelector& compound) {
    for (const PseudoClass& pseudo : compound.pseudos) {
        if (pseudo.pseudo_element) {
            if (!is_supported_pseudo_element(pseudo.name)) {
                return false;
            }
            continue;
        }
        if (!is_supported_pseudo_class(pseudo.name)) {
            return false;
        }
        if (pseudo.nested != nullptr && !list_supported(*pseudo.nested)) {
            return false;
        }
    }
    return true;
}

bool list_supported(const SelectorList& list) {
    return std::all_of(list.selectors.begin(), list.selectors.end(),
                       [](const ComplexSelector& selector) {
                           return std::all_of(selector.compounds.begin(), selector.compounds.end(),
                                              compound_supported);
                       });
}

} // namespace

bool selector_supported(std::string_view text) {
    const SelectorParseResult parsed = parse_selector_list(text);
    if (!parsed.ok || parsed.list.selectors.empty()) {
        return false;
    }
    return list_supported(parsed.list);
}

} // namespace tessera::ui::style
