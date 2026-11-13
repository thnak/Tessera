// SPDX-License-Identifier: MIT

#include "tessera/ui/style/selector_match.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

namespace tessera::ui::style {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool has_class(const StyleNode& node, std::string_view name) {
    return std::find(node.classes.begin(), node.classes.end(), name) != node.classes.end();
}

// Every name of a `::part(<ident>+)` argument must be present on the element (stylesheet-v1 6.3).
bool has_all_part_names(const PseudoClass& pseudo, const StyleNode& node) {
    if (node.part.empty()) {
        return false;
    }
    for (std::size_t index = 0;; ++index) {
        const std::string_view name = part_name_at(pseudo.argument, index);
        if (name.empty()) {
            return index != 0;
        }
        if (!has_part_name(node.part, name)) {
            return false;
        }
    }
}

const AttributeView* find_attribute(const StyleNode& node, std::string_view name) {
    for (const AttributeView& attribute : node.attributes) {
        if (attribute.name == name) {
            return &attribute;
        }
    }
    return nullptr;
}

bool equals(std::string_view lhs, std::string_view rhs, bool case_insensitive) {
    if (!case_insensitive) {
        return lhs == rhs;
    }
    return lower(lhs) == lower(rhs);
}

bool starts_with(std::string_view value, std::string_view prefix, bool case_insensitive) {
    if (prefix.size() > value.size()) {
        return false;
    }
    return equals(value.substr(0, prefix.size()), prefix, case_insensitive);
}

bool ends_with(std::string_view value, std::string_view suffix, bool case_insensitive) {
    if (suffix.size() > value.size()) {
        return false;
    }
    return equals(value.substr(value.size() - suffix.size()), suffix, case_insensitive);
}

bool contains_word(std::string_view value, std::string_view word, bool case_insensitive) {
    std::size_t start = 0;
    while (start < value.size()) {
        while (start < value.size() &&
               std::isspace(static_cast<unsigned char>(value[start])) != 0) {
            ++start;
        }
        std::size_t end = start;
        while (end < value.size() && std::isspace(static_cast<unsigned char>(value[end])) == 0) {
            ++end;
        }
        if (equals(value.substr(start, end - start), word, case_insensitive)) {
            return true;
        }
        start = end;
    }
    return false;
}

bool match_attribute(const AttributeSelector& selector, const StyleNode& node) {
    const AttributeView* attribute = find_attribute(node, selector.name);
    if (selector.op == AttributeOperator::Exists) {
        return attribute != nullptr;
    }
    if (attribute == nullptr) {
        return false;
    }

    switch (selector.op) {
    case AttributeOperator::Equals:
        return equals(attribute->value, selector.value, selector.case_insensitive);
    case AttributeOperator::Includes:
        return contains_word(attribute->value, selector.value, selector.case_insensitive);
    case AttributeOperator::Dash:
        return equals(attribute->value, selector.value, selector.case_insensitive) ||
               (attribute->value.size() > selector.value.size() &&
                equals(attribute->value.substr(0, selector.value.size()), selector.value,
                       selector.case_insensitive) &&
                attribute->value[selector.value.size()] == '-');
    case AttributeOperator::Prefix:
        return starts_with(attribute->value, selector.value, selector.case_insensitive);
    case AttributeOperator::Suffix:
        return ends_with(attribute->value, selector.value, selector.case_insensitive);
    case AttributeOperator::Substring:
        if (selector.case_insensitive) {
            return lower(attribute->value).find(lower(selector.value)) != std::string::npos;
        }
        return attribute->value.find(selector.value) != std::string_view::npos;
    case AttributeOperator::Exists:
        return true;
    }
    return false;
}

bool parse_int(std::string_view text, int& value) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parse_nth(std::string_view argument, int& a, int& b) {
    std::string compact;
    for (const char c : argument) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (compact.empty()) {
        return false;
    }
    if (compact == "odd") {
        a = 1;
        b = 1;
        return true;
    }
    if (compact == "even") {
        a = 2;
        b = 0;
        return true;
    }

    const std::size_t n = compact.find('n');
    if (n == std::string::npos) {
        a = 0;
        return parse_int(compact, b);
    }

    const std::string_view a_text(compact.data(), n);
    if (a_text.empty() || a_text == "+") {
        a = 1;
    } else if (a_text == "-") {
        a = -1;
    } else if (!parse_int(a_text, a)) {
        return false;
    }

    const std::string_view rest(compact.data() + n + 1, compact.size() - n - 1);
    if (rest.empty()) {
        b = 0;
        return true;
    }
    return parse_int(rest, b);
}

bool matches_nth(int a, int b, int index) {
    if (a == 0) {
        return index == b;
    }
    const int difference = index - b;
    if (a > 0 && difference < 0) {
        return false;
    }
    if (a < 0 && difference > 0) {
        return false;
    }
    return difference % a == 0;
}

int child_index(const StyleNode& node) {
    if (node.parent == nullptr) {
        return -1;
    }
    const auto& siblings = node.parent->children;
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i] == &node) {
            return static_cast<int>(i) + 1;
        }
    }
    return -1;
}

bool is_first_of_type(const StyleNode& node) {
    if (node.parent == nullptr) {
        return false;
    }
    for (const StyleNode* sibling : node.parent->children) {
        if (sibling == &node) {
            return true;
        }
        if (sibling->tag == node.tag) {
            return false;
        }
    }
    return false;
}

bool is_last_of_type(const StyleNode& node) {
    if (node.parent == nullptr) {
        return false;
    }
    bool seen = false;
    for (const StyleNode* sibling : node.parent->children) {
        if (sibling == &node) {
            seen = true;
        } else if (seen && sibling->tag == node.tag) {
            return false;
        }
    }
    return seen;
}

bool match_pseudo(const PseudoClass& pseudo, const StyleNode& node);

bool match_logical(const PseudoClass& pseudo, const StyleNode& node) {
    if (pseudo.nested == nullptr) {
        return false;
    }
    return matches(*pseudo.nested, node);
}

bool match_pseudo(const PseudoClass& pseudo, const StyleNode& node) {
    if (pseudo.pseudo_element || pseudo.name == "has") {
        return false;
    }
    if (pseudo.name == "hover") {
        return has_state(node.state, PseudoState::Hover);
    }
    if (pseudo.name == "active") {
        return has_state(node.state, PseudoState::Active);
    }
    if (pseudo.name == "focus") {
        return has_state(node.state, PseudoState::Focus);
    }
    if (pseudo.name == "focus-visible") {
        return has_state(node.state, PseudoState::FocusVisible);
    }
    if (pseudo.name == "disabled") {
        return has_state(node.state, PseudoState::Disabled);
    }
    if (pseudo.name == "enabled") {
        return !has_state(node.state, PseudoState::Disabled);
    }
    if (pseudo.name == "checked") {
        return has_state(node.state, PseudoState::Checked);
    }
    if (pseudo.name == "required") {
        return has_state(node.state, PseudoState::Required);
    }
    if (pseudo.name == "optional") {
        return !has_state(node.state, PseudoState::Required);
    }
    if (pseudo.name == "visited") {
        return false;
    }
    if (pseudo.name == "root") {
        return node.parent == nullptr;
    }
    if (pseudo.name == "empty") {
        return node.children.empty() && !node.has_text;
    }
    if (pseudo.name == "first-child") {
        return node.parent != nullptr && !node.parent->children.empty() &&
               node.parent->children.front() == &node;
    }
    if (pseudo.name == "last-child") {
        return node.parent != nullptr && !node.parent->children.empty() &&
               node.parent->children.back() == &node;
    }
    if (pseudo.name == "only-child") {
        return node.parent != nullptr && node.parent->children.size() == 1;
    }
    if (pseudo.name == "first-of-type") {
        return is_first_of_type(node);
    }
    if (pseudo.name == "last-of-type") {
        return is_last_of_type(node);
    }
    if (pseudo.name == "nth-child" || pseudo.name == "nth-last-child") {
        int a = 0;
        int b = 0;
        if (!parse_nth(pseudo.argument, a, b) || node.parent == nullptr) {
            return false;
        }
        int index = child_index(node);
        if (pseudo.name == "nth-last-child") {
            index = static_cast<int>(node.parent->children.size()) - index + 1;
        }
        return matches_nth(a, b, index);
    }
    if (pseudo.name == "not") {
        return !match_logical(pseudo, node);
    }
    if (pseudo.name == "is" || pseudo.name == "where") {
        return match_logical(pseudo, node);
    }
    return false;
}

// The host side of a `::part()` compound: the tag, id, classes, attributes and pseudo-classes
// written before the part pseudo-element all describe the component instance, not the part element.
// `part_index` is the position of `::part()` in `selector.pseudos`.
bool matches_host_side(const CompoundSelector& selector, const StyleNode& host,
                       std::size_t part_index) {
    if (!selector.tag.empty() && selector.tag != host.tag) {
        return false;
    }
    if (!selector.id.empty() && selector.id != host.id) {
        return false;
    }
    for (const std::string& name : selector.classes) {
        if (!has_class(host, name)) {
            return false;
        }
    }
    for (const AttributeSelector& attribute : selector.attributes) {
        if (!match_attribute(attribute, host)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < part_index; ++i) {
        if (!match_pseudo(selector.pseudos[i], host)) {
            return false;
        }
    }
    return true;
}

bool matches_compound(const CompoundSelector& selector, const StyleNode& node) {
    const std::size_t part_index = part_pseudo_index(selector);
    if (part_index != selector.pseudos.size()) {
        // Crossing the component boundary once: the node must be a part with every requested name,
        // it must sit inside a component, and the host side must match that nearest host. Only the
        // pseudo-classes written after `::part()` describe the part element itself.
        if (!has_all_part_names(selector.pseudos[part_index], node) || node.host == nullptr) {
            return false;
        }
        if (!matches_host_side(selector, *node.host, part_index)) {
            return false;
        }
        for (std::size_t i = part_index + 1; i < selector.pseudos.size(); ++i) {
            if (!match_pseudo(selector.pseudos[i], node)) {
                return false;
            }
        }
        return true;
    }

    if (!selector.tag.empty() && selector.tag != node.tag) {
        return false;
    }
    if (!selector.id.empty() && selector.id != node.id) {
        return false;
    }
    for (const std::string& name : selector.classes) {
        if (!has_class(node, name)) {
            return false;
        }
    }
    for (const AttributeSelector& attribute : selector.attributes) {
        if (!match_attribute(attribute, node)) {
            return false;
        }
    }
    for (const PseudoClass& pseudo : selector.pseudos) {
        if (!match_pseudo(pseudo, node)) {
            return false;
        }
    }
    return true;
}

bool match_at(const ComplexSelector& selector, std::size_t index, const StyleNode& node) {
    if (!matches_compound(selector.compounds[index], node)) {
        return false;
    }
    if (index == 0) {
        return true;
    }

    const Combinator combinator = selector.combinators[index - 1];
    if (node.parent == nullptr) {
        return false;
    }

    if (combinator == Combinator::Child) {
        return match_at(selector, index - 1, *node.parent);
    }
    if (combinator == Combinator::Descendant) {
        for (const StyleNode* ancestor = node.parent; ancestor != nullptr;
             ancestor = ancestor->parent) {
            if (match_at(selector, index - 1, *ancestor)) {
                return true;
            }
        }
        return false;
    }

    const auto& siblings = node.parent->children;
    const auto position = std::find(siblings.begin(), siblings.end(), &node);
    if (position == siblings.end()) {
        return false;
    }
    if (combinator == Combinator::AdjacentSibling) {
        return position != siblings.begin() && match_at(selector, index - 1, **(position - 1));
    }
    for (auto it = siblings.begin(); it != position; ++it) {
        if (match_at(selector, index - 1, **it)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool matches(const CompoundSelector& selector, const StyleNode& node) {
    return matches_compound(selector, node);
}

bool matches(const ComplexSelector& selector, const StyleNode& node) {
    if (selector.compounds.empty() ||
        selector.combinators.size() + 1 != selector.compounds.size()) {
        return false;
    }
    return match_at(selector, selector.compounds.size() - 1, node);
}

bool matches(const SelectorList& selectors, const StyleNode& node) {
    for (const ComplexSelector& selector : selectors.selectors) {
        if (matches(selector, node)) {
            return true;
        }
    }
    return false;
}

} // namespace tessera::ui::style
