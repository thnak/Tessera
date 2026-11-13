// SPDX-License-Identifier: MIT

// Component-part selectors (stylesheet-v1 section 6.3, ADR-047 item 3): the `::part(<ident>+)`
// helpers the parser, the selector index, the matcher and the diagnostic share. Split from
// selector.cpp (the parser and specificity) and selector_match.cpp (matching) to respect the file
// limits.

#include "tessera/ui/style/selector.hpp"

#include <cctype>
#include <cstddef>
#include <string_view>

namespace tessera::ui::style {

bool is_part_pseudo(const PseudoClass& pseudo) noexcept {
    return pseudo.pseudo_element && pseudo.name == "part";
}

std::string_view part_name_at(std::string_view names, std::size_t index) noexcept {
    std::size_t token = 0;
    std::size_t position = 0;
    while (position < names.size()) {
        while (position < names.size() &&
               std::isspace(static_cast<unsigned char>(names[position])) != 0) {
            ++position;
        }
        const std::size_t begin = position;
        while (position < names.size() &&
               std::isspace(static_cast<unsigned char>(names[position])) == 0) {
            ++position;
        }
        if (begin == position) {
            break;
        }
        if (token == index) {
            return names.substr(begin, position - begin);
        }
        ++token;
    }
    return {};
}

bool has_part_name(std::string_view parts, std::string_view name) noexcept {
    for (std::size_t index = 0;; ++index) {
        const std::string_view candidate = part_name_at(parts, index);
        if (candidate.empty()) {
            return false;
        }
        if (candidate == name) {
            return true;
        }
    }
}

bool has_part_pseudo(const CompoundSelector& compound) noexcept {
    return part_pseudo_index(compound) != compound.pseudos.size();
}

std::size_t part_pseudo_index(const CompoundSelector& compound) noexcept {
    for (std::size_t i = 0; i < compound.pseudos.size(); ++i) {
        if (is_part_pseudo(compound.pseudos[i])) {
            return i;
        }
    }
    return compound.pseudos.size();
}

} // namespace tessera::ui::style
