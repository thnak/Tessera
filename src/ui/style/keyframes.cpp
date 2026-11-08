// SPDX-License-Identifier: MIT

#include "tessera/ui/style/keyframes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trim(std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
                          return std::isspace(c) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::vector<std::string_view> split_commas(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ',') {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

bool parse_offset(std::string_view text, double& offset) {
    const std::string value = lower(trim(text));
    if (value == "from") {
        offset = 0.0;
        return true;
    }
    if (value == "to") {
        offset = 100.0;
        return true;
    }
    if (value.empty() || value.back() != '%') {
        return false;
    }
    const std::string number(value.substr(0, value.size() - 1));
    char* end = nullptr;
    offset = std::strtod(number.c_str(), &end);
    return end != nullptr && *end == '\0' && offset >= 0.0 && offset <= 100.0;
}

} // namespace

KeyframesParseResult parse_keyframes(const AtRule& at_rule) {
    KeyframesParseResult result;
    result.keyframes.name = trim(at_rule.prelude);
    if (!at_rule.has_block) {
        result.ok = false;
        return result;
    }

    const StyleSheet nested = parse_stylesheet(at_rule.block);
    for (const Rule& rule : nested.rules) {
        for (const std::string_view selector : split_commas(rule.selector)) {
            KeyframeStep step;
            if (!parse_offset(selector, step.offset)) {
                result.ok = false;
                continue;
            }
            step.declarations = rule.declarations;
            result.keyframes.steps.push_back(std::move(step));
        }
    }

    std::stable_sort(
        result.keyframes.steps.begin(), result.keyframes.steps.end(),
        [](const KeyframeStep& lhs, const KeyframeStep& rhs) { return lhs.offset < rhs.offset; });
    return result;
}

} // namespace tessera::ui::style
