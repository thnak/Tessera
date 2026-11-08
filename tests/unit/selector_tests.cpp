// SPDX-License-Identifier: MIT

#include "tessera/ui/style/selector.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::style::AttributeOperator;
using tessera::ui::style::Combinator;
using tessera::ui::style::parse_selector_list;
using tessera::ui::style::SelectorParseResult;
using tessera::ui::style::Specificity;
using tessera::ui::style::specificity;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "selector: " << message << "\n";
    }
    return condition;
}

bool spec(const Specificity& value, std::uint32_t ids, std::uint32_t classes, std::uint32_t types) {
    return value.ids == ids && value.classes == classes && value.types == types;
}

} // namespace

int main() {
    SelectorParseResult parsed = parse_selector_list(".card:hover > .title");
    if (!expect(parsed.ok, "parse hover child") ||
        !expect(parsed.list.selectors.size() == 1, "single complex selector")) {
        return 1;
    }
    const auto& complex = parsed.list.selectors[0];
    if (!expect(complex.compounds.size() == 2, "two compounds") ||
        !expect(complex.combinators.size() == 1, "one combinator") ||
        !expect(complex.combinators[0] == Combinator::Child, "child combinator") ||
        !expect(complex.compounds[0].classes.size() == 1, "first class") ||
        !expect(complex.compounds[0].classes[0] == "card", "card class") ||
        !expect(complex.compounds[0].pseudos.size() == 1, "hover pseudo") ||
        !expect(complex.compounds[0].pseudos[0].name == "hover", "hover name") ||
        !expect(complex.compounds[1].classes.size() == 1, "second class") ||
        !expect(spec(specificity(parsed.list), 0, 3, 0), "class/pseudo specificity")) {
        return 2;
    }

    parsed = parse_selector_list("#main .card[data-role=\"tab\"]:first-child");
    if (!expect(parsed.ok, "parse id class attribute") ||
        !expect(parsed.list.selectors[0].compounds.size() == 2, "descendant compounds") ||
        !expect(parsed.list.selectors[0].combinators[0] == Combinator::Descendant,
                "descendant combinator") ||
        !expect(parsed.list.selectors[0].compounds[0].id == "main", "id") ||
        !expect(parsed.list.selectors[0].compounds[1].attributes.size() == 1, "attribute") ||
        !expect(parsed.list.selectors[0].compounds[1].attributes[0].name == "data-role",
                "attribute name") ||
        !expect(parsed.list.selectors[0].compounds[1].attributes[0].op == AttributeOperator::Equals,
                "attribute equals") ||
        !expect(parsed.list.selectors[0].compounds[1].attributes[0].value == "tab",
                "attribute value") ||
        !expect(spec(specificity(parsed.list), 1, 3, 0), "attribute specificity")) {
        return 3;
    }

    parsed = parse_selector_list("button#save.primary::before");
    if (!expect(parsed.ok, "parse type id class pseudo-element") ||
        !expect(parsed.list.selectors[0].compounds[0].tag == "button", "type") ||
        !expect(parsed.list.selectors[0].compounds[0].pseudos[0].pseudo_element,
                "pseudo-element") ||
        !expect(spec(specificity(parsed.list), 1, 1, 2), "pseudo-element specificity")) {
        return 4;
    }

    parsed = parse_selector_list(".a > .b + .c ~ .d .e");
    if (!expect(parsed.ok, "parse all combinators") ||
        !expect(parsed.list.selectors[0].compounds.size() == 5, "five compounds") ||
        !expect(parsed.list.selectors[0].combinators.size() == 4, "four combinators") ||
        !expect(parsed.list.selectors[0].combinators[0] == Combinator::Child, "child") ||
        !expect(parsed.list.selectors[0].combinators[1] == Combinator::AdjacentSibling,
                "adjacent") ||
        !expect(parsed.list.selectors[0].combinators[2] == Combinator::GeneralSibling, "general") ||
        !expect(parsed.list.selectors[0].combinators[3] == Combinator::Descendant, "descendant")) {
        return 5;
    }

    parsed = parse_selector_list("[disabled] [lang|=en] [class~=\"x\"] [href^=\"https\"] "
                                 "[href$=\".pdf\"] [title*=\"hello\"]");
    if (!expect(parsed.ok, "parse attribute operators") ||
        !expect(parsed.list.selectors[0].compounds.size() == 6, "attribute compounds")) {
        return 6;
    }
    const auto& attributes = parsed.list.selectors[0].compounds;
    if (!expect(attributes[0].attributes[0].op == AttributeOperator::Exists, "exists") ||
        !expect(attributes[1].attributes[0].op == AttributeOperator::Dash, "dash") ||
        !expect(attributes[2].attributes[0].op == AttributeOperator::Includes, "includes") ||
        !expect(attributes[3].attributes[0].op == AttributeOperator::Prefix, "prefix") ||
        !expect(attributes[4].attributes[0].op == AttributeOperator::Suffix, "suffix") ||
        !expect(attributes[5].attributes[0].op == AttributeOperator::Substring, "substring")) {
        return 7;
    }

    parsed = parse_selector_list(":where(#id, .c) .x");
    if (!expect(parsed.ok, "parse where") ||
        !expect(parsed.list.selectors[0].compounds[0].pseudos[0].nested != nullptr,
                "where nested") ||
        !expect(spec(specificity(parsed.list), 0, 1, 0), "where zero specificity")) {
        return 8;
    }

    parsed = parse_selector_list(":is(#a, .b) .c");
    if (!expect(parsed.ok, "parse is") ||
        !expect(spec(specificity(parsed.list), 1, 1, 0), "is max specificity")) {
        return 9;
    }

    parsed = parse_selector_list(":not(.a, #b)");
    if (!expect(parsed.ok, "parse not") ||
        !expect(spec(specificity(parsed.list), 1, 0, 0), "not max specificity")) {
        return 10;
    }

    parsed = parse_selector_list(":nth-child(2n+1)");
    if (!expect(parsed.ok, "parse nth") ||
        !expect(parsed.list.selectors[0].compounds[0].pseudos[0].argument == "2n+1",
                "nth argument") ||
        !expect(spec(specificity(parsed.list), 0, 1, 0), "nth specificity")) {
        return 11;
    }

    parsed = parse_selector_list("*");
    if (!expect(parsed.ok, "parse universal") ||
        !expect(parsed.list.selectors[0].compounds[0].tag.empty(), "universal tag") ||
        !expect(spec(specificity(parsed.list), 0, 0, 0), "universal specificity")) {
        return 12;
    }

    parsed = parse_selector_list(".a, #b, tag");
    if (!expect(parsed.ok, "parse selector list") ||
        !expect(parsed.list.selectors.size() == 3, "list size") ||
        !expect(spec(specificity(parsed.list), 1, 0, 0), "list max specificity")) {
        return 13;
    }

    parsed = parse_selector_list("");
    if (!expect(!parsed.ok, "empty selector rejected")) {
        return 14;
    }

    parsed = parse_selector_list(".a >");
    if (!expect(!parsed.ok, "trailing combinator rejected")) {
        return 15;
    }

    parsed = parse_selector_list("[bad");
    if (!expect(!parsed.ok, "unterminated attribute rejected")) {
        return 16;
    }

    std::cout << "selector: ok\n";
    return 0;
}
