// SPDX-License-Identifier: MIT
//
// The tessera.json printer (ADR-061 slice 3): 2-space JSON (`indentWidth`), key order and duplicate
// keys preserved, string and number text kept byte for byte (only whitespace is decided), one
// trailing newline. An array of scalars stays on one line when it fits `printWidth`; objects and
// other arrays always break. The input must be strict JSON: there are no comments to keep.

#include "json_value.hpp"

#include <utility>
#include <vector>

namespace tessera::compiler::format {
namespace {

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
    if (!parse_json_value(source, root, problem)) {
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
    if (!parse_json_value(formatted, again, ignored) || (compact(again, b), a != b)) {
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
    if (!parse_json_value(text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text, root,
                          ignored) ||
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
