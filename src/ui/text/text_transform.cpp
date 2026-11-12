// SPDX-License-Identifier: MIT
//
// CSS `text-transform` (docs/design/text.md section 6.5). The V1 mapping is ASCII only and one byte
// to one byte, which is what keeps every line range, segment range, caret offset and selection
// offset computed on the original text valid for the transformed run. Non-ASCII bytes pass through
// untouched; full Unicode/ICU casing is a later slice, not a fallback here.

#include "tessera/ui/text/text_measure.hpp"

#include <string>
#include <string_view>

namespace tessera::ui::text {
namespace {

constexpr bool is_ascii_lower(char c) noexcept {
    return c >= 'a' && c <= 'z';
}

constexpr bool is_ascii_upper(char c) noexcept {
    return c >= 'A' && c <= 'Z';
}

constexpr char to_ascii_upper(char c) noexcept {
    return static_cast<char>(c - 'a' + 'A');
}

constexpr char to_ascii_lower(char c) noexcept {
    return static_cast<char>(c - 'A' + 'a');
}

// A word starts at the beginning of the run or immediately after one of these; the transform never
// rewrites them, so the separator test runs on the same bytes before and after the mapping.
constexpr bool is_word_separator(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n';
}

} // namespace

std::string apply_text_transform(std::string_view run, TextTransform transform) {
    std::string result(run);
    switch (transform) {
    case TextTransform::None:
        return result;
    case TextTransform::Uppercase:
        for (char& c : result) {
            if (is_ascii_lower(c)) {
                c = to_ascii_upper(c);
            }
        }
        return result;
    case TextTransform::Lowercase:
        for (char& c : result) {
            if (is_ascii_upper(c)) {
                c = to_ascii_lower(c);
            }
        }
        return result;
    case TextTransform::Capitalize: {
        bool word_start = true;
        for (char& c : result) {
            if (is_word_separator(c)) {
                word_start = true;
                continue;
            }
            if (!word_start) {
                continue;
            }
            // Digits, punctuation and non-ASCII bytes lead the word: skip them and uppercase the
            // first ASCII letter behind them. A letter that is already uppercase also ends the
            // lead-in, so the rest of the word is left alone.
            if (is_ascii_lower(c)) {
                c = to_ascii_upper(c);
                word_start = false;
            } else if (is_ascii_upper(c)) {
                word_start = false;
            }
        }
        return result;
    }
    }
    return result;
}

} // namespace tessera::ui::text
