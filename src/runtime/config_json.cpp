// SPDX-License-Identifier: MIT

#include "config_json.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace tessera::runtime::internal {
namespace {

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::uint32_t hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint32_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint32_t>(c - 'a') + 10U;
    }
    return static_cast<std::uint32_t>(c - 'A') + 10U;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

// First byte offset that is not well-formed UTF-8, or npos. Rejecting overlongs, surrogates and
// code points above U+10FFFF keeps the decoded string a valid UTF-8 sequence.
std::size_t invalid_utf8_offset(std::string_view text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = bytes[index];
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if (lead < 0x80U) {
            ++index;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = lead & 0x07U;
        } else {
            return index;
        }
        if (index + length > text.size()) {
            return index;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const unsigned char continuation = bytes[index + offset];
            if ((continuation & 0xC0U) != 0x80U) {
                return index;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        const bool overlong = (length == 2 && code_point < 0x80U) ||
                              (length == 3 && code_point < 0x800U) ||
                              (length == 4 && code_point < 0x10000U);
        const bool surrogate = code_point >= 0xD800U && code_point <= 0xDFFFU;
        if (overlong || surrogate || code_point > 0x10FFFFU) {
            return index;
        }
        index += length;
    }
    return std::string_view::npos;
}

class Parser {
public:
    Parser(std::string_view text, std::size_t max_depth)
        : text_(text),
          max_depth_(max_depth) {}

    std::expected<JsonValue, JsonParseError> parse_document() {
        const std::size_t invalid = invalid_utf8_offset(text_);
        if (invalid != std::string_view::npos) {
            return fail(JsonErrorCode::InvalidUtf8, invalid);
        }
        skip_whitespace();
        auto value = parse_value(0);
        if (!value) {
            return std::unexpected(value.error());
        }
        skip_whitespace();
        if (position_ != text_.size()) {
            return fail(JsonErrorCode::TrailingContent);
        }
        return value;
    }

private:
    std::expected<JsonValue, JsonParseError> parse_value(std::size_t depth) {
        skip_whitespace();
        if (position_ >= text_.size()) {
            return fail(JsonErrorCode::UnexpectedEnd);
        }
        const char c = text_[position_];
        switch (c) {
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"': {
            const std::size_t offset = position_;
            auto string = parse_string();
            if (!string) {
                return std::unexpected(string.error());
            }
            JsonValue value;
            value.type = JsonType::String;
            value.string = std::move(*string);
            value.offset = offset;
            return value;
        }
        case 't':
            return parse_bool_literal("true", true);
        case 'f':
            return parse_bool_literal("false", false);
        case 'n':
            return parse_literal("null");
        default:
            if (c == '-' || is_digit(c)) {
                return parse_number();
            }
            return fail(JsonErrorCode::UnexpectedCharacter);
        }
    }

    std::expected<JsonValue, JsonParseError> parse_bool_literal(std::string_view literal,
                                                                bool boolean) {
        const std::size_t offset = position_;
        if (text_.substr(position_, literal.size()) != literal) {
            return fail(JsonErrorCode::InvalidLiteral);
        }
        position_ += literal.size();
        JsonValue value;
        value.type = JsonType::Bool;
        value.boolean = boolean;
        value.offset = offset;
        return value;
    }

    std::expected<JsonValue, JsonParseError> parse_literal(std::string_view literal) {
        const std::size_t offset = position_;
        if (text_.substr(position_, literal.size()) != literal) {
            return fail(JsonErrorCode::InvalidLiteral);
        }
        position_ += literal.size();
        JsonValue value;
        value.type = JsonType::Null;
        value.offset = offset;
        return value;
    }

    std::expected<JsonValue, JsonParseError> parse_object(std::size_t depth) {
        if (depth >= max_depth_) {
            return fail(JsonErrorCode::NestingTooDeep);
        }
        const std::size_t offset = position_;
        ++position_; // '{'
        JsonValue value;
        value.type = JsonType::Object;
        value.offset = offset;
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return value;
        }
        for (;;) {
            if (position_ >= text_.size()) {
                return fail(JsonErrorCode::UnexpectedEnd);
            }
            if (text_[position_] != '"') {
                return fail(JsonErrorCode::UnexpectedCharacter);
            }
            const std::size_t key_offset = position_;
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_] != ':') {
                return fail(JsonErrorCode::UnexpectedCharacter);
            }
            ++position_;
            auto member = parse_value(depth + 1);
            if (!member) {
                return std::unexpected(member.error());
            }
            for (const auto& existing : value.object) {
                if (existing.first == *key) {
                    return fail(JsonErrorCode::DuplicateKey, key_offset);
                }
            }
            value.object.emplace_back(std::move(*key), std::move(*member));
            skip_whitespace();
            if (position_ >= text_.size()) {
                return fail(JsonErrorCode::UnexpectedEnd);
            }
            if (text_[position_] == ',') {
                ++position_;
                skip_whitespace();
                continue;
            }
            if (text_[position_] == '}') {
                ++position_;
                return value;
            }
            return fail(JsonErrorCode::UnexpectedCharacter);
        }
    }

    std::expected<JsonValue, JsonParseError> parse_array(std::size_t depth) {
        if (depth >= max_depth_) {
            return fail(JsonErrorCode::NestingTooDeep);
        }
        const std::size_t offset = position_;
        ++position_; // '['
        JsonValue value;
        value.type = JsonType::Array;
        value.offset = offset;
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return value;
        }
        for (;;) {
            auto element = parse_value(depth + 1);
            if (!element) {
                return std::unexpected(element.error());
            }
            value.array.push_back(std::move(*element));
            skip_whitespace();
            if (position_ >= text_.size()) {
                return fail(JsonErrorCode::UnexpectedEnd);
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == ']') {
                ++position_;
                return value;
            }
            return fail(JsonErrorCode::UnexpectedCharacter);
        }
    }

    std::expected<std::string, JsonParseError> parse_string() {
        ++position_; // '"'
        std::string out;
        for (;;) {
            if (position_ >= text_.size()) {
                return fail(JsonErrorCode::UnexpectedEnd);
            }
            const char c = text_[position_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                return fail(JsonErrorCode::UnexpectedCharacter, position_ - 1);
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) {
                return fail(JsonErrorCode::UnexpectedEnd);
            }
            const char escape = text_[position_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                const std::uint32_t code_point = parse_unicode_escape();
                if (code_point == kInvalidCodePoint) {
                    return fail(JsonErrorCode::InvalidEscape, position_ - 1);
                }
                append_utf8(out, code_point);
                break;
            }
            default:
                return fail(JsonErrorCode::InvalidEscape, position_ - 1);
            }
        }
    }

    static constexpr std::uint32_t kInvalidCodePoint = 0xFFFFFFFFU;

    std::uint32_t parse_hex4() {
        if (position_ + 4 > text_.size()) {
            return kInvalidCodePoint;
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char c = text_[position_ + static_cast<std::size_t>(index)];
            if (!is_hex_digit(c)) {
                return kInvalidCodePoint;
            }
            value = (value << 4U) | hex_value(c);
        }
        position_ += 4;
        return value;
    }

    std::uint32_t parse_unicode_escape() {
        const std::uint32_t first = parse_hex4();
        if (first == kInvalidCodePoint) {
            return kInvalidCodePoint;
        }
        if (first < 0xD800U || first > 0xDFFFU) {
            return first;
        }
        if (first > 0xDBFFU) {
            return kInvalidCodePoint; // lone low surrogate
        }
        if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
            text_[position_ + 1] != 'u') {
            return kInvalidCodePoint;
        }
        position_ += 2;
        const std::uint32_t second = parse_hex4();
        if (second == kInvalidCodePoint || second < 0xDC00U || second > 0xDFFFU) {
            return kInvalidCodePoint;
        }
        return 0x10000U + ((first - 0xD800U) << 10U) + (second - 0xDC00U);
    }

    std::expected<JsonValue, JsonParseError> parse_number() {
        const std::size_t offset = position_;
        bool exact_integer = true;
        if (text_[position_] == '-') {
            ++position_;
        }
        if (position_ >= text_.size()) {
            return fail(JsonErrorCode::UnexpectedEnd);
        }
        if (text_[position_] == '0') {
            ++position_;
        } else if (is_digit(text_[position_])) {
            while (position_ < text_.size() && is_digit(text_[position_])) {
                ++position_;
            }
        } else {
            return fail(JsonErrorCode::UnexpectedCharacter);
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            exact_integer = false;
            ++position_;
            if (position_ >= text_.size() || !is_digit(text_[position_])) {
                return fail(JsonErrorCode::UnexpectedCharacter);
            }
            while (position_ < text_.size() && is_digit(text_[position_])) {
                ++position_;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            exact_integer = false;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= text_.size() || !is_digit(text_[position_])) {
                return fail(JsonErrorCode::UnexpectedCharacter);
            }
            while (position_ < text_.size() && is_digit(text_[position_])) {
                ++position_;
            }
        }
        JsonValue value;
        value.type = JsonType::Number;
        value.exact_integer = exact_integer;
        value.offset = offset;
        const char* first = text_.data() + offset;
        const char* last = text_.data() + position_;
        const auto result = std::from_chars(first, last, value.number);
        if (result.ec == std::errc::result_out_of_range) {
            // JSON has no NaN/inf spelling; an overflowing literal is the only way a finite token
            // produces a non-finite value, and the loader reports it per field.
            value.non_finite = true;
            value.number = text_[offset] == '-' ? -std::numeric_limits<double>::infinity()
                                                : std::numeric_limits<double>::infinity();
        } else if (result.ec != std::errc() || result.ptr != last) {
            return fail(JsonErrorCode::InvalidNumber, offset);
        }
        return value;
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            ++position_;
        }
    }

    std::unexpected<JsonParseError> fail(JsonErrorCode code) const {
        return fail(code, position_);
    }
    std::unexpected<JsonParseError> fail(JsonErrorCode code, std::size_t offset) const {
        return std::unexpected(JsonParseError{code, offset});
    }

    std::string_view text_;
    std::size_t max_depth_ = 0;
    std::size_t position_ = 0;
};

} // namespace

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    for (const auto& member : object) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

std::expected<JsonValue, JsonParseError> parse_json(std::string_view text, std::size_t max_depth) {
    Parser parser(text, max_depth);
    return parser.parse_document();
}

} // namespace tessera::runtime::internal
