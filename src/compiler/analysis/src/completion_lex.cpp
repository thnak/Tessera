// SPDX-License-Identifier: MIT

#include "completion_lex.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace tessera::compiler::analysis::lex {

constexpr std::size_t kMaxTagScan = 4000; // bytes searched backwards for the open tag

bool ident_byte(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_' || c == '$' || u >= 0x80;
}

bool ident_start(char c) {
    return ident_byte(c) && std::isdigit(static_cast<unsigned char>(c)) == 0;
}

bool starts_with_icase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           std::ranges::equal(text.substr(0, prefix.size()), prefix, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

std::size_t line_start(std::string_view text, std::size_t at) {
    const auto nl = at == 0 ? std::string_view::npos : text.rfind('\n', at - 1);
    return nl == std::string_view::npos ? 0 : nl + 1;
}

std::size_t skip_space_back(std::string_view text, std::size_t at) {
    while (at > 0 && (text[at - 1] == ' ' || text[at - 1] == '\t' || text[at - 1] == '\n' ||
                      text[at - 1] == '\r')) {
        --at;
    }
    return at;
}

std::string_view word_before(std::string_view text, std::size_t end) {
    std::size_t begin = end;
    while (begin > 0 && ident_byte(text[begin - 1])) {
        --begin;
    }
    return text.substr(begin, end - begin);
}

bool odd_count(std::string_view text, char quote) {
    return std::ranges::count(text, quote) % 2 == 1;
}

// The JSX tag whose attribute list holds byte `at`, or none when an expression container, a
// closed tag or a statement boundary comes first. Best effort on text that is mid-edit.
std::optional<OpenTag> open_tag(std::string_view text, std::size_t at) {
    int braces = 0;
    const std::size_t floor = at > kMaxTagScan ? at - kMaxTagScan : 0;
    for (std::size_t i = at; i-- > floor;) {
        const char c = text[i];
        if (c == '"' || c == '\'') {
            const std::size_t ls = line_start(text, i);
            const auto prev = text.substr(ls, i - ls).rfind(c);
            if (prev != std::string_view::npos) {
                i = ls + prev;
            }
        } else if (c == '}') {
            ++braces;
        } else if (c == '{') {
            if (braces == 0) {
                return std::nullopt;
            }
            --braces;
        } else if (braces == 0 && c == '>') {
            if (i > 0 && text[i - 1] == '=') {
                continue;
            }
            return std::nullopt;
        } else if (braces == 0 && c == ';') {
            return std::nullopt;
        } else if (braces == 0 && c == '<') {
            std::size_t end = i + 1;
            while (end < text.size() && (ident_byte(text[end]) || text[end] == '.')) {
                ++end;
            }
            if (end == i + 1) {
                return std::nullopt;
            }
            const std::size_t prev = skip_space_back(text, i);
            const bool jsx =
                prev == 0 ||
                (!ident_byte(text[prev - 1]) && text[prev - 1] != ')' && text[prev - 1] != ']') ||
                word_before(text, prev) == "return";
            if (!jsx) {
                continue;
            }
            return OpenTag{std::string(text.substr(i + 1, end - i - 1)), end};
        }
    }
    return std::nullopt;
}

// Attribute names already written in the tag head (depth 0, outside strings).
std::set<std::string> written_attributes(std::string_view head) {
    std::set<std::string> out;
    int braces = 0;
    for (std::size_t i = 0; i < head.size(); ++i) {
        const char c = head[i];
        if (c == '"' || c == '\'') {
            const auto end = head.find(c, i + 1);
            i = end == std::string_view::npos ? head.size() : end;
        } else if (c == '{') {
            ++braces;
        } else if (c == '}') {
            --braces;
        } else if (braces == 0 && ident_start(c)) {
            std::size_t end = i;
            while (end < head.size() && (ident_byte(head[end]) || head[end] == '-')) {
                ++end;
            }
            out.insert(std::string(head.substr(i, end - i)));
            i = end - 1;
        }
    }
    return out;
}

std::string snake_case(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (std::isupper(static_cast<unsigned char>(c)) != 0) {
            if (i > 0 && std::islower(static_cast<unsigned char>(name[i - 1])) != 0) {
                out += '_';
            }
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

// '/' separated path of `target` relative to the directory of `from`, as an import specifier.
std::string specifier(std::string_view from, std::string_view target) {
    std::vector<std::string_view> a;
    std::vector<std::string_view> b;
    const auto split = [](std::string_view p, std::vector<std::string_view>& parts) {
        for (std::size_t i = 0; i <= p.size();) {
            const auto j = p.find('/', i);
            const auto end = j == std::string_view::npos ? p.size() : j;
            parts.push_back(p.substr(i, end - i));
            i = end + 1;
        }
    };
    split(from, a);
    split(target, b);
    a.pop_back(); // the file name of the importer
    std::size_t common = 0;
    while (common < a.size() && common + 1 < b.size() && a[common] == b[common]) {
        ++common;
    }
    std::string out = a.size() == common ? "./" : "";
    for (std::size_t i = common; i < a.size(); ++i) {
        out += "../";
    }
    for (std::size_t i = common; i < b.size(); ++i) {
        out += i == common ? "" : "/";
        out += b[i];
    }
    for (const std::string_view ext : {".tsx", ".ts"}) {
        if (out.ends_with(ext)) {
            out.resize(out.size() - ext.size());
            break;
        }
    }
    return out;
}

} // namespace tessera::compiler::analysis::lex
