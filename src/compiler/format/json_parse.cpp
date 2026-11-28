// SPDX-License-Identifier: MIT
//
// The strict JSON parser behind the tessera.json printer (ADR-061 slice 3). Strings and numbers
// keep their raw text, object key order and duplicate keys are preserved, nesting is bounded.

#include "json_value.hpp"

namespace tessera::compiler::format {
namespace {

constexpr int kMaxDepth = 128;

class Parser {
public:
    explicit Parser(std::string_view s)
        : s_(s) {}

    bool parse(Value& out, Problem& problem) {
        skip();
        if (!value(out, 0) || (skip(), pos_ != s_.size())) {
            if (error_.empty()) {
                error_ = "unexpected text after the value";
                at_ = pos_;
            }
            problem = Problem{"TESSERA_FMT_SYNTAX", "invalid JSON: " + error_, at_};
            return false;
        }
        return true;
    }

private:
    bool fail(std::string message) {
        error_ = std::move(message);
        at_ = pos_;
        return false;
    }

    void skip() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word, Value& out) {
        if (s_.substr(pos_, word.size()) != word) {
            return fail("unexpected character");
        }
        out.raw = std::string(word);
        pos_ += word.size();
        return true;
    }

    bool digits() {
        const std::size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            ++pos_;
        }
        return pos_ > start;
    }

    bool number(Value& out) {
        const std::size_t start = pos_;
        if (s_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ < s_.size() && s_[pos_] == '0') {
            ++pos_;
        } else if (!digits()) {
            return fail("malformed number");
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (!digits()) {
                return fail("malformed number");
            }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            if (!digits()) {
                return fail("malformed number");
            }
        }
        out.raw = std::string(s_.substr(start, pos_ - start));
        return true;
    }

    bool string(std::string& raw) {
        const std::size_t start = pos_++;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c < 0x20) {
                return fail("control character in a string");
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) {
                    break;
                }
                if (s_[pos_] == 'u') {
                    for (int k = 1; k <= 4; ++k) {
                        const char h = pos_ + k < s_.size() ? s_[pos_ + k] : '\0';
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                              (h >= 'A' && h <= 'F'))) {
                            return fail("malformed \\u escape");
                        }
                    }
                    pos_ += 4;
                } else if (std::string_view("\"\\/bfnrt").find(s_[pos_]) ==
                           std::string_view::npos) {
                    return fail("unknown escape");
                }
            }
            ++pos_;
        }
        if (pos_ >= s_.size()) {
            return fail("unterminated string");
        }
        ++pos_;
        raw = std::string(s_.substr(start, pos_ - start));
        return true;
    }

    bool value(Value& out, int depth) {
        if (pos_ >= s_.size()) {
            return fail("unexpected end of the file");
        }
        if (depth > kMaxDepth) {
            return fail("nesting is too deep");
        }
        const char c = s_[pos_];
        if (c == '{') {
            out.kind = Value::Kind::Object;
            ++pos_;
            skip();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                skip();
                std::pair<std::string, Value> member;
                if (pos_ >= s_.size() || s_[pos_] != '"') {
                    return fail("expected a string key");
                }
                if (!string(member.first)) {
                    return false;
                }
                skip();
                if (pos_ >= s_.size() || s_[pos_] != ':') {
                    return fail("expected `:`");
                }
                ++pos_;
                skip();
                if (!value(member.second, depth + 1)) {
                    return false;
                }
                out.members.push_back(std::move(member));
                skip();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return fail("expected `,` or `}`");
            }
        }
        if (c == '[') {
            out.kind = Value::Kind::Array;
            ++pos_;
            skip();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                skip();
                Value item;
                if (!value(item, depth + 1)) {
                    return false;
                }
                out.items.push_back(std::move(item));
                skip();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return fail("expected `,` or `]`");
            }
        }
        if (c == '"') {
            return string(out.raw);
        }
        if (c == 't') {
            return literal("true", out);
        }
        if (c == 'f') {
            return literal("false", out);
        }
        if (c == 'n') {
            return literal("null", out);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return number(out);
        }
        return fail("unexpected character");
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    std::size_t at_ = 0;
    std::string error_;
};

} // namespace

bool parse_json_value(std::string_view source, Value& out, Problem& problem) {
    return Parser(source).parse(out, problem);
}

} // namespace tessera::compiler::format
