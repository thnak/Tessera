// SPDX-License-Identifier: MIT

// Strict JSON reader for the shader manifest. The model and the rationale for a schema-local
// reader are in json.hpp.

#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>
#include <utility>

namespace tessera::schema::internal {

namespace {

bool is_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

int hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

class Parser {
public:
    Parser(std::string_view text, std::size_t max_depth) noexcept
        : text_(text),
          max_depth_(max_depth) {}

    std::expected<JsonValue, JsonParseError> parse() {
        skip_whitespace();
        if (at_end()) {
            return fail(JsonErrorCode::UnexpectedEnd, index_);
        }
        auto value = parse_value(0);
        if (!value) {
            return value;
        }
        skip_whitespace();
        if (!at_end()) {
            return fail(JsonErrorCode::TrailingContent, index_);
        }
        return value;
    }

private:
    static std::unexpected<JsonParseError> fail(JsonErrorCode code, std::size_t offset) {
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

    bool consume(char expected) noexcept {
        if (at_end() || peek() != expected) {
            return false;
        }
        ++index_;
        return true;
    }

    std::expected<JsonValue, JsonParseError> parse_value(std::size_t depth) {
        if (at_end()) {
            return fail(JsonErrorCode::UnexpectedEnd, index_);
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
            return JsonValue::make_string(std::move(*value));
        }
        case 't':
            return parse_literal("true", JsonValue::make_bool(true));
        case 'f':
            return parse_literal("false", JsonValue::make_bool(false));
        case 'n':
            return parse_literal("null", JsonValue{});
        default:
            if (peek() == '-' || is_digit(peek())) {
                return parse_number();
            }
            return fail(JsonErrorCode::UnexpectedCharacter, index_);
        }
    }

    std::expected<JsonValue, JsonParseError> parse_literal(std::string_view literal,
                                                           JsonValue value) {
        if (text_.substr(index_, literal.size()) != literal) {
            return fail(JsonErrorCode::InvalidLiteral, index_);
        }
        index_ += literal.size();
        return value;
    }

    std::expected<JsonValue, JsonParseError> parse_object(std::size_t depth) {
        if (depth >= max_depth_) {
            return fail(JsonErrorCode::NestingTooDeep, index_);
        }
        ++index_; // '{'
        JsonValue::Object members;
        skip_whitespace();
        if (consume('}')) {
            return JsonValue::make_object(std::move(members));
        }
        while (true) {
            skip_whitespace();
            if (at_end()) {
                return fail(JsonErrorCode::UnexpectedEnd, index_);
            }
            if (peek() != '"') {
                return fail(JsonErrorCode::UnexpectedCharacter, index_);
            }
            const std::size_t key_offset = index_;
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            for (const auto& member : members) {
                if (member.first == *key) {
                    return fail(JsonErrorCode::DuplicateKey, key_offset);
                }
            }
            skip_whitespace();
            if (!consume(':')) {
                return at_end() ? fail(JsonErrorCode::UnexpectedEnd, index_)
                                : fail(JsonErrorCode::UnexpectedCharacter, index_);
            }
            skip_whitespace();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            members.emplace_back(std::move(*key), std::move(*value));
            skip_whitespace();
            if (consume('}')) {
                return JsonValue::make_object(std::move(members));
            }
            if (!consume(',')) {
                return at_end() ? fail(JsonErrorCode::UnexpectedEnd, index_)
                                : fail(JsonErrorCode::UnexpectedCharacter, index_);
            }
        }
    }

    std::expected<JsonValue, JsonParseError> parse_array(std::size_t depth) {
        if (depth >= max_depth_) {
            return fail(JsonErrorCode::NestingTooDeep, index_);
        }
        ++index_; // '['
        JsonValue::Array values;
        skip_whitespace();
        if (consume(']')) {
            return JsonValue::make_array(std::move(values));
        }
        while (true) {
            skip_whitespace();
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            values.push_back(std::move(*value));
            skip_whitespace();
            if (consume(']')) {
                return JsonValue::make_array(std::move(values));
            }
            if (!consume(',')) {
                return at_end() ? fail(JsonErrorCode::UnexpectedEnd, index_)
                                : fail(JsonErrorCode::UnexpectedCharacter, index_);
            }
        }
    }

    std::expected<std::string, JsonParseError> parse_string() {
        ++index_; // '"'
        std::string out;
        while (true) {
            if (at_end()) {
                return fail(JsonErrorCode::UnexpectedEnd, index_);
            }
            const auto character = static_cast<unsigned char>(peek());
            if (character == '"') {
                ++index_;
                break;
            }
            if (character < 0x20) {
                return fail(JsonErrorCode::ControlCharacter, index_);
            }
            if (character != '\\') {
                out.push_back(static_cast<char>(character));
                ++index_;
                continue;
            }
            ++index_;
            if (at_end()) {
                return fail(JsonErrorCode::UnexpectedEnd, index_);
            }
            if (!parse_escape(out)) {
                return fail(JsonErrorCode::InvalidEscape, index_);
            }
        }
        if (!is_valid_utf8(out)) {
            return fail(JsonErrorCode::InvalidUtf8, index_);
        }
        return out;
    }

    bool parse_escape(std::string& out) {
        const char escape = peek();
        ++index_;
        switch (escape) {
        case '"':
            out.push_back('"');
            return true;
        case '\\':
            out.push_back('\\');
            return true;
        case '/':
            out.push_back('/');
            return true;
        case 'b':
            out.push_back('\b');
            return true;
        case 'f':
            out.push_back('\f');
            return true;
        case 'n':
            out.push_back('\n');
            return true;
        case 'r':
            out.push_back('\r');
            return true;
        case 't':
            out.push_back('\t');
            return true;
        case 'u':
            return parse_unicode_escape(out);
        default:
            return false;
        }
    }

    bool read_hex4(std::uint32_t& value) {
        if (index_ + 4 > text_.size()) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(text_[index_ + static_cast<std::size_t>(i)]);
            if (digit < 0) {
                return false;
            }
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        index_ += 4;
        return true;
    }

    bool parse_unicode_escape(std::string& out) {
        std::uint32_t code_point = 0;
        if (!read_hex4(code_point)) {
            return false;
        }
        if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            return false; // a low surrogate without its high half
        }
        if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            if (index_ + 2 > text_.size() || text_[index_] != '\\' || text_[index_ + 1] != 'u') {
                return false;
            }
            index_ += 2;
            std::uint32_t low = 0;
            if (!read_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                return false;
            }
            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(out, code_point);
        return true;
    }

    std::expected<JsonValue, JsonParseError> parse_number() {
        const std::size_t start = index_;
        if (peek() == '-') {
            ++index_;
        }
        if (at_end()) {
            return fail(JsonErrorCode::InvalidNumber, start);
        }
        if (peek() == '0') {
            ++index_;
            if (!at_end() && is_digit(peek())) {
                return fail(JsonErrorCode::InvalidNumber, start);
            }
        } else if (is_digit(peek())) {
            while (!at_end() && is_digit(peek())) {
                ++index_;
            }
        } else {
            return fail(JsonErrorCode::InvalidNumber, start);
        }

        bool integer_literal = true;
        if (!at_end() && peek() == '.') {
            integer_literal = false;
            ++index_;
            if (at_end() || !is_digit(peek())) {
                return fail(JsonErrorCode::InvalidNumber, start);
            }
            while (!at_end() && is_digit(peek())) {
                ++index_;
            }
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            integer_literal = false;
            ++index_;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                ++index_;
            }
            if (at_end() || !is_digit(peek())) {
                return fail(JsonErrorCode::InvalidNumber, start);
            }
            while (!at_end() && is_digit(peek())) {
                ++index_;
            }
        }

        const std::string_view text = text_.substr(start, index_ - start);
        const char* first = text.data();
        const char* last = text.data() + text.size();
        if (integer_literal) {
            std::int64_t value = 0;
            const auto result = std::from_chars(first, last, value);
            if (result.ec == std::errc::result_out_of_range) {
                return fail(JsonErrorCode::NumberOutOfRange, start);
            }
            if (result.ec != std::errc() || result.ptr != last) {
                return fail(JsonErrorCode::InvalidNumber, start);
            }
            return JsonValue::make_integer(value);
        }
        double value = 0.0;
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc() || result.ptr != last || !std::isfinite(value)) {
            return fail(JsonErrorCode::NumberOutOfRange, start);
        }
        return JsonValue::make_number(value);
    }

    std::string_view text_;
    std::size_t index_ = 0;
    std::size_t max_depth_ = 0;
};

} // namespace

std::expected<JsonValue, JsonParseError> parse_json(std::string_view text, std::size_t max_depth) {
    Parser parser(text, max_depth);
    return parser.parse();
}

} // namespace tessera::schema::internal
