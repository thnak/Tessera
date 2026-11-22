// SPDX-License-Identifier: MIT
//
// The small text helpers the CSS grammar needs, in one place (issue #733).
//
// Before this header the tree held 26 copies of `lower`, 24 of `trim` and 8 of `split_top_level`,
// spread over ~30 translation units. The cost was never the duplicated bytes: it was that a fix to
// one copy (a UTF-8 boundary, a `\f`, a locale-dependent `std::tolower`) silently left the other 25
// wrong, and the CSS grammar ended up with a private dialect per file.
//
// Two rules the implementations here keep, and the copies did not:
//
// 1. **ASCII-only, locale-free.** `std::tolower` reads the current C locale, so a Turkish locale
//    maps 'I' to a dotless 'i' and a stylesheet parses differently depending on the user's
//    environment. CSS keywords are ASCII, so the mapping is done arithmetically and never consults
//    the locale.
// 2. **One whitespace set.** CSS whitespace is space, tab, CR, LF and FF. The copies disagreed:
//    some trimmed only " \t", some " \t\r\n\f", so a value with a form feed parsed in one file and
//    not in another.
//
// Everything here is allocation-light and header-only: `trim` returns a view, `lower` allocates one
// string, and `split_top_level` allocates one vector of views into the caller's text.

#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tessera::detail {

// The characters CSS treats as whitespace (CSS Syntax 3 section 3.3). Deliberately not
// `std::isspace`, which also accepts vertical tab and is locale-dependent.
inline constexpr std::string_view kCssWhitespace = " \t\r\n\f";

// ASCII-only lower-casing. A byte outside 'A'..'Z' is copied unchanged, so a UTF-8 continuation
// byte is never reinterpreted and the result is the same in every locale.
[[nodiscard]] inline char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// A lower-cased copy of `text`. Allocates once; the caller owns the result.
[[nodiscard]] inline std::string lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = ascii_lower(c);
    }
    return result;
}

// `text` without leading or trailing CSS whitespace. Returns a view into `text`, so the caller must
// keep it alive; an all-whitespace input yields an empty view.
[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept {
    const std::size_t first = text.find_first_not_of(kCssWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kCssWhitespace);
    return text.substr(first, last - first + 1);
}

// `text` trimmed and lower-cased, which is the pair almost every keyword comparison wants.
[[nodiscard]] inline std::string normalized(std::string_view text) {
    return lower(trim(text));
}

// Splits `text` on `separator` at parenthesis depth 0, so a separator inside a function call stays
// inside its part. The parts are views into `text` and are NOT trimmed: a caller that wants trimmed
// parts maps `trim` over them, because trimming is a grammar decision (a gradient's stop list wants
// it, a `calc()` argument may not).
//
// An unbalanced ')' is ignored rather than driving the depth negative, so a malformed value splits
// the same way a well-formed one would instead of collapsing into one part.
[[nodiscard]] inline std::vector<std::string_view> split_top_level(std::string_view text,
                                                                   char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth > 0) {
                --depth;
            }
        } else if (c == separator && depth == 0) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

// Splits on top-level commas and trims each part, which is the `background-image` / `font-family`
// shape. Empty parts are KEPT: a caller that treats an empty part as a grammar error (a
// `background-image` with a doubled comma) needs to see it, and a caller that does not can skip it.
[[nodiscard]] inline std::vector<std::string_view> split_commas(std::string_view text) {
    std::vector<std::string_view> parts;
    for (const std::string_view part : split_top_level(text, ',')) {
        parts.push_back(trim(part));
    }
    return parts;
}

// Parses a leading number and returns the unconsumed suffix (the unit), or an empty view when the
// text does not start with a number. `value` is only written on success.
[[nodiscard]] inline std::string_view parse_number(std::string_view text, double& value) noexcept {
    std::size_t index = 0;
    // `std::from_chars` rejects a leading '+', so it is consumed here and the sign is applied
    // after.
    bool negative = false;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        negative = text[index] == '-';
        ++index;
    }
    const std::size_t digits_start = index;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        ++index;
    }
    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
        }
    }
    if (index == digits_start) {
        return {};
    }
    // `from_chars` is locale-free and does not allocate, which is why it is used instead of
    // `strtod` (which reads the C locale's decimal point).
    const std::string_view number = text.substr(digits_start, index - digits_start);
    double parsed = 0.0;
    const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
    if (result.ec != std::errc{}) {
        return {};
    }
    value = negative ? -parsed : parsed;
    return text.substr(index);
}

} // namespace tessera::detail
