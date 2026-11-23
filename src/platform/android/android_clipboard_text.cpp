// SPDX-License-Identifier: MIT

#include "android_clipboard_text.hpp"

namespace tessera::hal::android {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Decodes one UTF-8 sequence. Returns the code point and advances `index` past it; an invalid or
// truncated sequence yields U+FFFD and consumes exactly one byte, so the walk always terminates and
// never loses a byte.
char32_t decode_utf8(std::string_view text, std::size_t& index) noexcept {
    const auto byte = [&](std::size_t offset) {
        return static_cast<std::uint8_t>(text[index + offset]);
    };
    const std::uint8_t lead = byte(0);
    std::size_t length = 0;
    char32_t code = 0;
    if (lead < 0x80U) {
        ++index;
        return lead;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        code = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        code = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        code = lead & 0x07U;
    } else {
        ++index;
        return kReplacement;
    }
    if (index + length > text.size()) {
        ++index;
        return kReplacement;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const std::uint8_t continuation = byte(offset);
        if ((continuation & 0xC0U) != 0x80U) {
            ++index;
            return kReplacement;
        }
        code = (code << 6U) | (continuation & 0x3FU);
    }
    // Overlong encodings, surrogates and out-of-range values are not valid UTF-8; report them as
    // U+FFFD rather than passing a value a UTF-16 encoder cannot represent.
    const bool overlong = (length == 2 && code < 0x80U) || (length == 3 && code < 0x800U) ||
                          (length == 4 && code < 0x10000U);
    const bool surrogate = code >= 0xD800U && code <= 0xDFFFU;
    if (overlong || surrogate || code > 0x10FFFFU) {
        ++index;
        return kReplacement;
    }
    index += length;
    return code;
}

void append_utf8(std::string& out, char32_t code) {
    if (code < 0x80U) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else if (code < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
}

} // namespace

std::size_t utf16_length_from_utf8(std::string_view text) noexcept {
    std::size_t units = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const char32_t code = decode_utf8(text, index);
        units += code > 0xFFFFU ? 2U : 1U;
    }
    return units;
}

std::u16string utf8_to_utf16(std::string_view text) {
    std::u16string out;
    out.reserve(utf16_length_from_utf8(text));
    std::size_t index = 0;
    while (index < text.size()) {
        const char32_t code = decode_utf8(text, index);
        if (code <= 0xFFFFU) {
            out.push_back(static_cast<char16_t>(code));
            continue;
        }
        // A code point above the BMP is a surrogate pair: two UTF-16 units, not one.
        const char32_t adjusted = code - 0x10000U;
        out.push_back(static_cast<char16_t>(0xD800U + (adjusted >> 10U)));
        out.push_back(static_cast<char16_t>(0xDC00U + (adjusted & 0x3FFU)));
    }
    return out;
}

std::string utf16_to_utf8(std::u16string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char16_t unit = text[index];
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            // A high surrogate needs its low half. A lone one becomes U+FFFD, the same rule the IME
            // path applies, so the two directions agree on malformed input.
            if (index + 1 < text.size() && text[index + 1] >= 0xDC00U &&
                text[index + 1] <= 0xDFFFU) {
                const char32_t code = 0x10000U + ((static_cast<char32_t>(unit) - 0xD800U) << 10U) +
                                      (static_cast<char32_t>(text[index + 1]) - 0xDC00U);
                append_utf8(out, code);
                ++index;
                continue;
            }
            append_utf8(out, kReplacement);
            continue;
        }
        if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            // A lone low surrogate has no high half to pair with.
            append_utf8(out, kReplacement);
            continue;
        }
        append_utf8(out, unit);
    }
    return out;
}

} // namespace tessera::hal::android
