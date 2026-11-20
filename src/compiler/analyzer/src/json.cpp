// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/json.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

namespace tessera::compiler::jsx::json {
namespace {

class Parser {
public:
    Parser(std::string_view text, Error* error)
        : text_(text),
          error_(error) {}

    std::optional<Value> run() {
        skip_space();
        std::optional<Value> value = parse_value(0);
        if (!value) {
            return std::nullopt;
        }
        skip_space();
        if (position_ != text_.size()) {
            return fail(ErrorCode::TrailingContent);
        }
        return value;
    }

private:
    std::nullopt_t fail(ErrorCode code) {
        if (error_ != nullptr) {
            error_->code = code;
            error_->offset = position_;
        }
        return std::nullopt;
    }

    void skip_space() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    std::optional<Value> parse_value(std::size_t depth) {
        if (depth > kMaxDepth) {
            return fail(ErrorCode::DepthExceeded);
        }
        if (position_ >= text_.size()) {
            return fail(ErrorCode::UnexpectedEnd);
        }
        switch (text_[position_]) {
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"':
            return parse_string();
        case 't':
            return parse_literal("true", Value(true));
        case 'f':
            return parse_literal("false", Value(false));
        case 'n':
            return parse_literal("null", Value());
        default:
            return parse_number();
        }
    }

    std::optional<Value> parse_literal(std::string_view literal, Value value) {
        if (text_.substr(position_, literal.size()) != literal) {
            return fail(ErrorCode::UnexpectedCharacter);
        }
        position_ += literal.size();
        return value;
    }

    std::optional<Value> parse_object(std::size_t depth) {
        ++position_; // '{'
        Value::Object members;
        skip_space();
        if (consume('}')) {
            return Value(std::move(members));
        }
        while (true) {
            skip_space();
            if (position_ >= text_.size()) {
                return fail(ErrorCode::UnexpectedEnd);
            }
            if (text_[position_] != '"') {
                return fail(ErrorCode::UnexpectedCharacter);
            }
            std::optional<Value> key = parse_string();
            if (!key) {
                return std::nullopt;
            }
            const std::string name(*key->as_string());
            for (const Member& member : members) {
                if (member.first == name) {
                    return fail(ErrorCode::DuplicateKey);
                }
            }
            skip_space();
            if (!consume(':')) {
                return fail(ErrorCode::UnexpectedCharacter);
            }
            skip_space();
            std::optional<Value> value = parse_value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            members.emplace_back(name, std::move(*value));
            skip_space();
            if (consume('}')) {
                return Value(std::move(members));
            }
            if (!consume(',')) {
                return fail(ErrorCode::UnexpectedCharacter);
            }
        }
    }

    std::optional<Value> parse_array(std::size_t depth) {
        ++position_; // '['
        Value::Array elements;
        skip_space();
        if (consume(']')) {
            return Value(std::move(elements));
        }
        while (true) {
            skip_space();
            std::optional<Value> value = parse_value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            elements.push_back(std::move(*value));
            skip_space();
            if (consume(']')) {
                return Value(std::move(elements));
            }
            if (!consume(',')) {
                return fail(ErrorCode::UnexpectedCharacter);
            }
        }
    }

    // Appends the code point at `position_` as UTF-8. The input is already UTF-8, so a raw byte is
    // copied as it is; only `\uXXXX` needs encoding.
    static void append_utf8(std::string& out, std::uint32_t code_point) {
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

    std::optional<std::uint32_t> parse_hex4() {
        if (position_ + 4 > text_.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char c = text_[position_ + static_cast<std::size_t>(index)];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        position_ += 4;
        return value;
    }

    std::optional<Value> parse_string() {
        ++position_; // '"'
        std::string out;
        while (true) {
            if (position_ >= text_.size()) {
                return fail(ErrorCode::UnexpectedEnd);
            }
            const char c = text_[position_++];
            if (c == '"') {
                return Value(std::move(out));
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) {
                return fail(ErrorCode::UnexpectedEnd);
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
                const std::optional<std::uint32_t> code_point = parse_hex4();
                if (!code_point) {
                    return fail(ErrorCode::InvalidEscape);
                }
                append_utf8(out, *code_point);
                break;
            }
            default:
                return fail(ErrorCode::InvalidEscape);
            }
        }
    }

    std::optional<Value> parse_number() {
        const std::size_t start = position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) {
            ++position_;
        }
        bool digits = false;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            ++position_;
            digits = true;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
                digits = true;
            }
        }
        if (digits && position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) {
                ++position_;
            }
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
        }
        if (!digits) {
            return fail(ErrorCode::UnexpectedCharacter);
        }
        // `strtod` needs a null-terminated buffer; the slice is short and this runs once per
        // manifest, so a copy is cheaper than a hand-written float parser.
        const std::string slice(text_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(slice.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(value)) {
            return fail(ErrorCode::InvalidNumber);
        }
        return Value(value);
    }

    std::string_view text_;
    Error* error_ = nullptr;
    std::size_t position_ = 0;
};

} // namespace

const Value* Value::find(std::string_view key) const noexcept {
    const Object* members = as_object();
    if (members == nullptr) {
        return nullptr;
    }
    for (const Member& member : *members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

std::optional<Value> parse(std::string_view text, Error* error) {
    Parser parser(text, error);
    return parser.run();
}

} // namespace tessera::compiler::jsx::json
