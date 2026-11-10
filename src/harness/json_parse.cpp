// SPDX-License-Identifier: MIT

#include "json_string_internal.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/utf8.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace tessera::harness {
namespace {

bool is_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

class Parser {
public:
    Parser(std::string_view text, std::size_t max_depth) noexcept
        : text_(text),
          max_depth_(max_depth) {}

    std::expected<JsonValue, JsonParseError> parse() {
        skip_whitespace();
        if (at_end()) {
            return fail(JsonParseErrorCode::UnexpectedEnd, index_);
        }
        auto value = parse_value(0);
        if (!value) {
            return value;
        }
        skip_whitespace();
        if (!at_end()) {
            return fail(JsonParseErrorCode::TrailingContent, index_);
        }
        return value;
    }

private:
    std::unexpected<JsonParseError> fail(JsonParseErrorCode code, std::size_t offset) const {
        return std::unexpected(JsonParseError{code, offset});
    }

    [[nodiscard]] bool at_end() const noexcept {
        return index_ >= text_.size();
    }
    [[nodiscard]] char peek() const noexcept {
        return text_[index_];
    }

    void skip_whitespace() noexcept {
        while (!at_end()) {
            const char character = peek();
            if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
                break;
            }
            ++index_;
        }
    }

    std::expected<JsonValue, JsonParseError> parse_value(std::size_t depth) {
        if (at_end()) {
            return fail(JsonParseErrorCode::UnexpectedEnd, index_);
        }
        switch (peek()) {
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"': {
            auto value = parse_string();
            if (!value) {
                return std::unexpected(value.error());
            }
            return JsonValue::string(std::move(*value));
        }
        case 't':
            return parse_literal("true", JsonValue::boolean(true));
        case 'f':
            return parse_literal("false", JsonValue::boolean(false));
        case 'n':
            return parse_literal("null", JsonValue());
        default:
            break;
        }
        if (peek() == '-' || is_digit(peek())) {
            return parse_number();
        }
        return fail(JsonParseErrorCode::UnexpectedCharacter, index_);
    }

    std::expected<JsonValue, JsonParseError> parse_literal(std::string_view literal,
                                                           JsonValue value) {
        for (const char expected : literal) {
            if (at_end() || peek() != expected) {
                return fail(JsonParseErrorCode::InvalidLiteral, index_);
            }
            ++index_;
        }
        // A bare `truex` must not decode as `true` followed by trailing content.
        if (!at_end() && (is_digit(peek()) || (peek() >= 'a' && peek() <= 'z') ||
                          (peek() >= 'A' && peek() <= 'Z'))) {
            return fail(JsonParseErrorCode::InvalidLiteral, index_);
        }
        return value;
    }

    std::expected<JsonValue, JsonParseError> parse_object(std::size_t depth) {
        if (depth >= max_depth_) {
            return fail(JsonParseErrorCode::NestingTooDeep, index_);
        }
        ++index_; // '{'
        JsonValue::Object members;
        skip_whitespace();
        if (at_end()) {
            return fail(JsonParseErrorCode::UnexpectedEnd, index_);
        }
        if (peek() == '}') {
            ++index_;
            return JsonValue::object(std::move(members));
        }
        for (;;) {
            skip_whitespace();
            if (at_end()) {
                return fail(JsonParseErrorCode::UnexpectedEnd, index_);
            }
            if (peek() != '"') {
                return fail(JsonParseErrorCode::UnexpectedCharacter, index_);
            }
            const std::size_t key_offset = index_;
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            for (const JsonValue::Member& member : members) {
                if (member.first == *key) {
                    return fail(JsonParseErrorCode::DuplicateKey, key_offset);
                }
            }
            skip_whitespace();
            if (at_end()) {
                return fail(JsonParseErrorCode::UnexpectedEnd, index_);
            }
            if (peek() != ':') {
                return fail(JsonParseErrorCode::UnexpectedCharacter, index_);
            }
            ++index_;
            skip_whitespace();
            auto member = parse_value(depth + 1);
            if (!member) {
                return member;
            }
            members.emplace_back(std::move(*key), std::move(*member));
            skip_whitespace();
            if (at_end()) {
                return fail(JsonParseErrorCode::UnexpectedEnd, index_);
            }
            if (peek() == ',') {
                ++index_;
                continue;
            }
            if (peek() == '}') {
                ++index_;
                break;
            }
            return fail(JsonParseErrorCode::UnexpectedCharacter, index_);
        }
        return JsonValue::object(std::move(members));
    }

    std::expected<JsonValue, JsonParseError> parse_array(std::size_t depth) {
        if (depth >= max_depth_) {
            return fail(JsonParseErrorCode::NestingTooDeep, index_);
        }
        ++index_; // '['
        JsonValue::Array values;
        skip_whitespace();
        if (at_end()) {
            return fail(JsonParseErrorCode::UnexpectedEnd, index_);
        }
        if (peek() == ']') {
            ++index_;
            return JsonValue::array(std::move(values));
        }
        for (;;) {
            skip_whitespace();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            values.push_back(std::move(*value));
            skip_whitespace();
            if (at_end()) {
                return fail(JsonParseErrorCode::UnexpectedEnd, index_);
            }
            if (peek() == ',') {
                ++index_;
                continue;
            }
            if (peek() == ']') {
                ++index_;
                break;
            }
            return fail(JsonParseErrorCode::UnexpectedCharacter, index_);
        }
        return JsonValue::array(std::move(values));
    }

    std::expected<std::string, JsonParseError> parse_string() {
        return detail::decode_json_string(text_, index_);
    }

    std::expected<JsonValue, JsonParseError> parse_number() {
        const std::size_t start = index_;
        bool negative = false;
        if (peek() == '-') {
            negative = true;
            ++index_;
        }
        if (at_end()) {
            return fail(JsonParseErrorCode::UnexpectedEnd, index_);
        }
        if (!is_digit(peek())) {
            return fail(JsonParseErrorCode::InvalidNumber, index_);
        }
        // JSON forbids leading zeros: 0 is fine, 01 is not.
        if (peek() == '0') {
            ++index_;
        } else {
            while (!at_end() && is_digit(peek())) {
                ++index_;
            }
        }
        bool integral = true;
        if (!at_end() && peek() == '.') {
            integral = false;
            ++index_;
            if (at_end() || !is_digit(peek())) {
                return fail(JsonParseErrorCode::InvalidNumber, index_);
            }
            while (!at_end() && is_digit(peek())) {
                ++index_;
            }
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            integral = false;
            ++index_;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                ++index_;
            }
            if (at_end() || !is_digit(peek())) {
                return fail(JsonParseErrorCode::InvalidNumber, index_);
            }
            while (!at_end() && is_digit(peek())) {
                ++index_;
            }
        }
        const std::string_view literal = text_.substr(start, index_ - start);
        if (!integral) {
            double value = 0.0;
            const auto result = std::from_chars(literal.data(), literal.data() + literal.size(),
                                                value, std::chars_format::general);
            if (result.ec == std::errc::result_out_of_range || !std::isfinite(value)) {
                return fail(JsonParseErrorCode::NumberOutOfRange, start);
            }
            if (result.ec != std::errc{} || result.ptr != literal.data() + literal.size()) {
                return fail(JsonParseErrorCode::InvalidNumber, start);
            }
            return JsonValue::number(value);
        }
        constexpr std::uint64_t kInt64Max =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        std::uint64_t magnitude = 0;
        for (const char character : literal) {
            if (character == '-') {
                continue;
            }
            const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
            if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                return fail(JsonParseErrorCode::NumberOutOfRange, start);
            }
            magnitude = magnitude * 10U + digit;
        }
        const std::uint64_t limit = negative ? kInt64Max + 1U : kInt64Max;
        if (magnitude > limit) {
            return fail(JsonParseErrorCode::NumberOutOfRange, start);
        }
        if (!negative) {
            return JsonValue::integer(static_cast<std::int64_t>(magnitude));
        }
        if (magnitude == kInt64Max + 1U) {
            return JsonValue::integer(std::numeric_limits<std::int64_t>::min());
        }
        return JsonValue::integer(-static_cast<std::int64_t>(magnitude));
    }

    std::string_view text_;
    std::size_t index_ = 0;
    std::size_t max_depth_;
};

} // namespace

std::expected<JsonValue, JsonParseError> parse_json(std::string_view text, std::size_t max_depth) {
    std::size_t bad_offset = 0;
    if (!is_valid_utf8(text, &bad_offset)) {
        return std::unexpected(JsonParseError{JsonParseErrorCode::InvalidUtf8, bad_offset});
    }
    Parser parser(text, max_depth);
    return parser.parse();
}

} // namespace tessera::harness
