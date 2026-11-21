// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

struct Declaration {
    std::string property;
    std::string value;
    bool important = false;
    std::size_t offset = 0;
};

// `alignas(64)` (issue #732): `std::string` (32) + `std::vector` (24) + `std::size_t` (8) is
// exactly 64, so with the natural 8-byte alignment seven of eight rules straddle a line and a walk
// over `std::vector<Rule>` touches both. The alignment changes no offset.
struct alignas(64) Rule {
    std::string selector;
    std::vector<Declaration> declarations;
    std::size_t offset = 0;
};

struct AtRule {
    std::string name;
    std::string prelude;
    std::string block;
    bool has_block = false;
    std::size_t offset = 0;
};

struct StyleSheet {
    std::vector<Rule> rules;
    std::vector<AtRule> at_rules;
};

StyleSheet parse_stylesheet(std::string_view source);

} // namespace tessera::ui::style
