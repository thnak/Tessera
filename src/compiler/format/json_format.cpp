// SPDX-License-Identifier: MIT
//
// The tessera.json printer (ADR-061 slice 3): 2-space JSON (`indentWidth`), key order and duplicate
// keys preserved, string and number text kept byte for byte (only whitespace is decided), one
// trailing newline. An array of scalars stays on one line when it fits `printWidth`; objects and
// other arrays always break. The input must be strict JSON: there are no comments to keep.

#include "other_formats.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace tessera::compiler::format {
namespace {

constexpr int kMaxDepth = 128;

struct Value {
    enum class Kind : std::uint8_t { Scalar, Object, Array } kind = Kind::Scalar;
    std::string raw; // scalar text, or an object key's raw string
    std::vector<std::pair<std::string, Value>> members;
    std::vector<Value> items;
};

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

std::size_t columns(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return n;
}

class Printer {
public:
    explicit Printer(const FormatOptions& o)
        : indent_(static_cast<std::size_t>(o.indent_width < 1 ? 2 : o.indent_width)),
          width_(static_cast<std::size_t>(o.print_width < 1 ? 100 : o.print_width)) {}

    void print(const Value& v, std::size_t depth, std::size_t column, std::string& out) const {
        switch (v.kind) {
        case Value::Kind::Scalar:
            out += v.raw;
            return;
        case Value::Kind::Object:
            if (v.members.empty()) {
                out += "{}";
                return;
            }
            out += "{\n";
            for (std::size_t i = 0; i < v.members.size(); ++i) {
                const auto& [key, child] = v.members[i];
                out.append((depth + 1) * indent_, ' ');
                out += key + ": ";
                print(child, depth + 1, (depth + 1) * indent_ + columns(key) + 2, out);
                out += i + 1 < v.members.size() ? ",\n" : "\n";
            }
            out.append(depth * indent_, ' ');
            out += '}';
            return;
        case Value::Kind::Array:
            print_array(v, depth, column, out);
            return;
        }
    }

private:
    void print_array(const Value& v, std::size_t depth, std::size_t column,
                     std::string& out) const {
        if (v.items.empty()) {
            out += "[]";
            return;
        }
        bool scalars = true;
        std::string flat = "[";
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            scalars = scalars && v.items[i].kind == Value::Kind::Scalar;
            flat += (i > 0 ? ", " : "") + v.items[i].raw;
        }
        flat += ']';
        if (scalars && column + columns(flat) + 1 <= width_) {
            out += flat;
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            out.append((depth + 1) * indent_, ' ');
            print(v.items[i], depth + 1, (depth + 1) * indent_, out);
            out += i + 1 < v.items.size() ? ",\n" : "\n";
        }
        out.append(depth * indent_, ' ');
        out += ']';
    }

    std::size_t indent_;
    std::size_t width_;
};

// Structure and text without whitespace: equal for two documents that mean the same thing.
void compact(const Value& v, std::string& out) {
    switch (v.kind) {
    case Value::Kind::Scalar:
        out += v.raw;
        return;
    case Value::Kind::Object:
        out += '{';
        for (const auto& [key, child] : v.members) {
            out += key + ":";
            compact(child, out);
            out += ',';
        }
        out += '}';
        return;
    case Value::Kind::Array:
        out += '[';
        for (const Value& item : v.items) {
            compact(item, out);
            out += ',';
        }
        out += ']';
        return;
    }
}

bool int_member(const Value& object, std::string_view name, long min, long max, long& out) {
    for (const auto& [key, child] : object.members) {
        if (key == "\"" + std::string(name) + "\"" && child.kind == Value::Kind::Scalar &&
            !child.raw.empty() && child.raw.find_first_not_of("0123456789") == std::string::npos &&
            child.raw.size() <= 4) {
            const long n = std::stol(child.raw);
            if (n >= min && n <= max) {
                out = n;
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool format_json(std::string_view source, const FormatOptions& options, std::string& out,
                 Problem& problem) {
    Value root;
    if (!Parser(source).parse(root, problem)) {
        return false;
    }
    std::string formatted;
    Printer(options).print(root, 0, 0, formatted);
    formatted += '\n';

    Value again;
    Problem ignored;
    std::string a;
    std::string b;
    compact(root, a);
    if (!Parser(formatted).parse(again, ignored) || (compact(again, b), a != b)) {
        problem = Problem{"TESSERA_FMT_VERIFY",
                          "the printed output does not mean the same as the input; this is a "
                          "formatter bug",
                          0};
        return false;
    }
    out = std::move(formatted);
    return true;
}

FormatOptions format_options_from_tessera_json(std::string_view text, FormatOptions base) {
    Value root;
    Problem ignored;
    if (!Parser(text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text).parse(root, ignored) ||
        root.kind != Value::Kind::Object) {
        return base;
    }
    for (const auto& [key, child] : root.members) {
        if (key == "\"format\"" && child.kind == Value::Kind::Object) {
            long n = 0;
            if (int_member(child, "printWidth", 40, 400, n)) {
                base.print_width = static_cast<int>(n);
            }
            if (int_member(child, "indentWidth", 1, 8, n)) {
                base.indent_width = static_cast<int>(n);
            }
        }
    }
    return base;
}

} // namespace tessera::compiler::format
