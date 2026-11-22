// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/keyframes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {

using tessera::detail::lower;
using tessera::detail::split_commas;
namespace {

std::string trim(std::string_view value) {
    return std::string(tessera::detail::trim(value));
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
