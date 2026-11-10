// SPDX-License-Identifier: MIT

#include "json_string_internal.hpp"
#include "tessera/harness/utf8.hpp"

#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

namespace tessera::harness::detail {
namespace {

bool is_hex(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

char32_t hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<char32_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<char32_t>(value - 'a' + 10);
    }
    return static_cast<char32_t>(value - 'A' + 10);
}

std::unexpected<JsonParseError> fail(JsonParseErrorCode code, std::size_t offset) {
    return std::unexpected(JsonParseError{code, offset});
}

// Four hexadecimal digits, the payload of a `\u` escape.
std::expected<char16_t, JsonParseError> decode_hex4(std::string_view text, std::size_t& index) {
    char32_t value = 0;
    for (int digit = 0; digit < 4; ++digit) {
        if (index >= text.size()) {
            return fail(JsonParseErrorCode::UnexpectedEnd, index);
        }
        const char character = text[index++];
        if (!is_hex(character)) {
            return fail(JsonParseErrorCode::InvalidUtf16Escape, index - 1);
        }
        value = value * 16 + hex_value(character);
    }
    return static_cast<char16_t>(value);
}

} // namespace

std::expected<std::string, JsonParseError> decode_json_string(std::string_view text,
                                                              std::size_t& index) {
    ++index; // opening quote
    std::string out;
    out.reserve(32);
    for (;;) {
        if (index >= text.size()) {
            return fail(JsonParseErrorCode::UnexpectedEnd, index);
        }
        const char character = text[index];
        if (character == '"') {
            ++index;
            return out;
        }
        if (static_cast<unsigned char>(character) < 0x20U) {
            return fail(JsonParseErrorCode::ControlCharacterInString, index);
        }
        if (character != '\\') {
            out.push_back(character);
            ++index;
            continue;
        }
        const std::size_t escape_offset = index;
        ++index;
        if (index >= text.size()) {
            return fail(JsonParseErrorCode::UnexpectedEnd, index);
        }
        const char escape = text[index++];
        switch (escape) {
        case '"':
            out.push_back('"');
            continue;
        case '\\':
            out.push_back('\\');
            continue;
        case '/':
            out.push_back('/');
            continue;
        case 'b':
            out.push_back('\b');
            continue;
        case 'f':
            out.push_back('\f');
            continue;
        case 'n':
            out.push_back('\n');
            continue;
        case 'r':
            out.push_back('\r');
            continue;
        case 't':
            out.push_back('\t');
            continue;
        case 'u':
            break;
        default:
            return fail(JsonParseErrorCode::InvalidEscape, escape_offset + 1);
        }
        auto unit = decode_hex4(text, index);
        if (!unit) {
            return std::unexpected(unit.error());
        }
        char32_t code_point = *unit;
        if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            // A high surrogate is only legal as the first half of a UTF-16 pair.
            if (index >= text.size() || text[index] != '\\') {
                return fail(JsonParseErrorCode::InvalidUtf16Escape, escape_offset);
            }
            ++index;
            if (index >= text.size() || text[index] != 'u') {
                return fail(JsonParseErrorCode::InvalidUtf16Escape, escape_offset);
            }
            ++index;
            auto low = decode_hex4(text, index);
            if (!low) {
                return std::unexpected(low.error());
            }
            if (*low < 0xDC00U || *low > 0xDFFFU) {
                return fail(JsonParseErrorCode::InvalidUtf16Escape, escape_offset);
            }
            code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                         (static_cast<char32_t>(*low) - 0xDC00U);
        } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            return fail(JsonParseErrorCode::InvalidUtf16Escape, escape_offset);
        }
        if (!append_utf8(out, code_point)) {
            return fail(JsonParseErrorCode::InvalidUtf16Escape, escape_offset);
        }
    }
}

} // namespace tessera::harness::detail
