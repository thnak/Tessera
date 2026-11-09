// SPDX-License-Identifier: MIT

#pragma once

#include <cctype>
#include <string_view>

// Internal string helpers shared by the accessibility implementation files. Not installed. They are
// allocation-free and ASCII-only: role/style keywords and ARIA values are ASCII by contract.
namespace tessera::ui::a11y::detail {

inline std::string_view trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

inline char lower_ascii(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

inline bool equal_ci(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lower_ascii(lhs[index]) != lower_ascii(rhs[index])) {
            return false;
        }
    }
    return true;
}

inline bool is_true(std::string_view value) {
    return equal_ci(trim(value), "true");
}

} // namespace tessera::ui::a11y::detail
