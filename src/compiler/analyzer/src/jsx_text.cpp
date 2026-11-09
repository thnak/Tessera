// SPDX-License-Identifier: MIT

#include "jsx_text.hpp"
#include "utf8.hpp"

namespace tessera::compiler::jsx {
namespace {

bool is_entity_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#';
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Returns false when `body` (the text between `&#` and `;`) is not a Unicode scalar value.
bool numeric_entity(std::string_view body, char32_t& value) {
    unsigned base = 10;
    if (!body.empty() && (body[0] == 'x' || body[0] == 'X')) {
        base = 16;
        body.remove_prefix(1);
    }
    if (body.empty()) {
        return false;
    }
    unsigned long accumulated = 0;
    for (const char c : body) {
        const int digit = hex_digit(c);
        if (digit < 0 || static_cast<unsigned>(digit) >= base) {
            return false;
        }
        accumulated = accumulated * base + static_cast<unsigned long>(digit);
        if (accumulated > 0x10FFFFUL) {
            return false;
        }
    }
    if (accumulated == 0 || (accumulated >= 0xD800UL && accumulated <= 0xDFFFUL)) {
        return false;
    }
    value = static_cast<char32_t>(accumulated);
    return true;
}

} // namespace

std::string decode_entities(std::string_view text, std::vector<EntityError>* errors) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out.push_back(text[i++]);
            continue;
        }
        // `&name;` with a bounded name; anything else is a literal ampersand.
        constexpr std::size_t kMaxNameLength = 12;
        std::size_t end = i + 1;
        while (end < text.size() && end - i - 1 <= kMaxNameLength && is_entity_char(text[end])) {
            ++end;
        }
        if (end == i + 1 || end >= text.size() || text[end] != ';' ||
            end - i - 1 > kMaxNameLength) {
            out.push_back(text[i++]);
            continue;
        }
        const std::string_view whole = text.substr(i, end + 1 - i);
        const std::string_view name = text.substr(i + 1, end - i - 1);
        bool ok = true;
        if (name == "amp") {
            out.push_back('&');
        } else if (name == "lt") {
            out.push_back('<');
        } else if (name == "gt") {
            out.push_back('>');
        } else if (name == "quot") {
            out.push_back('"');
        } else if (name == "apos") {
            out.push_back('\'');
        } else if (name == "nbsp") {
            append_utf8(out, 0xA0);
        } else if (name[0] == '#') {
            char32_t value = 0;
            ok = numeric_entity(name.substr(1), value);
            if (ok) {
                append_utf8(out, value);
            }
        } else {
            ok = false;
        }
        if (!ok) {
            if (errors != nullptr) {
                const bool numeric = name[0] == '#';
                errors->push_back(EntityError{
                    i, whole.size(),
                    (numeric ? "invalid numeric character reference `" : "unknown entity `") +
                        std::string(whole) + "`"});
            }
            out.append(whole);
        }
        i = end + 1;
    }
    return out;
}

std::string collapse_jsx_text(std::string_view raw) {
    std::vector<std::string_view> lines;
    for (std::size_t begin = 0;;) {
        std::size_t end = begin;
        while (end < raw.size() && raw[end] != '\n') {
            ++end;
        }
        std::string_view line = raw.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (end >= raw.size()) {
            break;
        }
        begin = end + 1;
    }
    std::size_t last_non_blank = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        for (const char c : lines[i]) {
            if (c != ' ' && c != '\t') {
                last_non_blank = i;
                break;
            }
        }
    }
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line(lines[i]);
        for (char& c : line) {
            if (c == '\t') {
                c = ' ';
            }
        }
        std::size_t first = 0;
        std::size_t last = line.size();
        if (i != 0) {
            while (first < last && line[first] == ' ') {
                ++first;
            }
        }
        if (i + 1 != lines.size()) {
            while (last > first && line[last - 1] == ' ') {
                --last;
            }
        }
        if (first == last) {
            continue;
        }
        out.append(line, first, last - first);
        if (i != last_non_blank) {
            out.push_back(' ');
        }
    }
    return out;
}

} // namespace tessera::compiler::jsx
