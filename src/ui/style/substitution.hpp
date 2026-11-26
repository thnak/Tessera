// SPDX-License-Identifier: MIT

#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

namespace tessera::ui::style::detail {

// True when `text` holds a `var(` or `env(` call. Both are substituted at computed-value time,
// after parse-time validation, so a validator that cannot judge the text yet keeps the declaration.
// The match is ASCII-case-insensitive, like CSS function names, and also matches inside a longer
// identifier (`somevar(`), which only ever makes a validator more lenient.
[[nodiscard]] inline bool has_substitution_function(std::string_view text) {
    const auto fold = [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    };
    for (std::size_t i = 0; i + 4 <= text.size(); ++i) {
        if (text[i + 3] != '(') {
            continue;
        }
        const char first = fold(text[i]);
        const char second = fold(text[i + 1]);
        const char third = fold(text[i + 2]);
        if ((first == 'v' && second == 'a' && third == 'r') ||
            (first == 'e' && second == 'n' && third == 'v')) {
            return true;
        }
    }
    return false;
}

} // namespace tessera::ui::style::detail
