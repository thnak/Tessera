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

struct Rule {
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
