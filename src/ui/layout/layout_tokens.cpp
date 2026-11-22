// SPDX-License-Identifier: MIT
//
// The CSS token helpers `apply_style` uses to read a computed value: trim, ASCII lower-case, a
// keyword comparison, and a finite-float parse. Split out of `layout_style.cpp`, which is at its
// source-size budget.
//
// `equals_keyword` exists because the obvious `lower(trim(*value)) == keyword` form materialises
// two `std::string`s per call, and `apply_style` asks the question ~32 times per node. That was 64
// allocations per node per layout pass - the largest single allocation source in a size-change
// frame (issue #735). The comparison below trims and lower-cases in place, so it allocates nothing.

#include "layout_style.hpp"
#include "tessera/detail/text_util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace tessera::ui::layout::detail {

bool is_keyword(const std::string* value, std::string_view keyword) {
    return value != nullptr && equals_keyword(*value, keyword);
}

bool equals_keyword(std::string_view value, std::string_view keyword) noexcept {
    const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    std::size_t first = 0;
    std::size_t last = value.size();
    while (first < last && is_space(value[first])) {
        ++first;
    }
    while (last > first && is_space(value[last - 1])) {
        --last;
    }
    if (last - first != keyword.size()) {
        return false;
    }
    for (std::size_t i = 0; i < keyword.size(); ++i) {
        const char c = value[first + i];
        const char lowered = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (lowered != keyword[i]) {
            return false;
        }
    }
    return true;
}

bool parse_number(std::string_view text, float& value) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(token.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace tessera::ui::layout::detail
