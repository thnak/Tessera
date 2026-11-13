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

// `::part(<ident>+)`: the component part pseudo-element (stylesheet-v1 section 6.3, ADR-047 item
// 3). It is always a pseudo-element written with two colons and takes one or more
// whitespace-separated part names in its argument; every one of them must be present on the element
// for the compound to match. It is the compound's single boundary to its component host: the
// compound's other simple selectors match the host (the nearest ancestor component instance), while
// pseudo-classes written after it match the part element itself.
[[nodiscard]] bool is_part_pseudo(const PseudoClass& pseudo) noexcept;

// The `index`-th name of a parsed `::part()` argument, or an empty view past the end.
// Allocation-free so both the selector index and the matcher walk the names without building a
// list.
[[nodiscard]] std::string_view part_name_at(std::string_view names, std::size_t index) noexcept;

// Whether the whitespace-separated `part` list `parts` contains `name` (exact and case-sensitive,
// as in CSS Shadow Parts). Allocation-free.
[[nodiscard]] bool has_part_name(std::string_view parts, std::string_view name) noexcept;

// Whether `compound` carries a `::part()` pseudo-element.
[[nodiscard]] bool has_part_pseudo(const CompoundSelector& compound) noexcept;

// The position of the `::part()` pseudo-element in `compound.pseudos`; `pseudos.size()` when there
// is none. The position splits the compound: simple selectors before it match the host, the ones
// after it match the part element.
[[nodiscard]] std::size_t part_pseudo_index(const CompoundSelector& compound) noexcept;

// Whether the engine can match with `text` (stylesheet-v1 section 6.4): the selector list parses
// and every pseudo-class/pseudo-element it uses has implemented matching semantics. `@supports
// selector(...)` answers with this.
[[nodiscard]] bool selector_supported(std::string_view text);

Specificity specificity(const CompoundSelector& selector);
Specificity specificity(const ComplexSelector& selector);
Specificity specificity(const SelectorList& selectors);

} // namespace tessera::ui::style
