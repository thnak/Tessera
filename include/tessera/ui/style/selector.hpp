// SPDX-License-Identifier: MIT

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

enum class Combinator : std::uint8_t {
    Descendant,
    Child,
    AdjacentSibling,
    GeneralSibling,
};

enum class AttributeOperator : std::uint8_t {
    Exists,
    Equals,
    Includes,
    Dash,
    Prefix,
    Suffix,
    Substring,
};

struct AttributeSelector {
    std::string name;
    AttributeOperator op = AttributeOperator::Exists;
    std::string value;
    bool case_insensitive = false;
};

struct Specificity {
    std::uint32_t ids = 0;
    std::uint32_t classes = 0;
    std::uint32_t types = 0;

    auto operator<=>(const Specificity&) const = default;

    Specificity& operator+=(const Specificity& other) {
        ids += other.ids;
        classes += other.classes;
        types += other.types;
        return *this;
    }
};

struct SelectorList;

struct PseudoClass {
    std::string name;
    std::string argument;
    std::shared_ptr<const SelectorList> nested;
    bool pseudo_element = false;
};

struct CompoundSelector {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::vector<AttributeSelector> attributes;
    std::vector<PseudoClass> pseudos;
};

struct ComplexSelector {
    std::vector<CompoundSelector> compounds;
    std::vector<Combinator> combinators;
};

struct SelectorList {
    std::vector<ComplexSelector> selectors;
};

struct SelectorParseResult {
    SelectorList list;
    bool ok = true;
    std::size_t error_offset = 0;
};

SelectorParseResult parse_selector_list(std::string_view text);

Specificity specificity(const CompoundSelector& selector);
Specificity specificity(const ComplexSelector& selector);
Specificity specificity(const SelectorList& selectors);

} // namespace tessera::ui::style
