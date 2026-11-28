// SPDX-License-Identifier: MIT

#include "css_scan.hpp"
#include "utf16_map.hpp"

#include <cstddef>

namespace tessera::compiler::analysis {

namespace {

bool is_name_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c >= 0x80;
}

bool starts_name(unsigned char c) {
    return is_name_byte(c) && !(c >= '0' && c <= '9');
}

} // namespace

std::vector<CssClassSite> scan_css_classes(std::string_view css) {
    const Utf16Map map(css);
    std::vector<CssClassSite> out;
    int declaration_depth = 0;
    std::vector<bool> stack; // true: a declaration block, false: an at-rule group
    bool at_rule = false;
    bool seen = false; // a non-space byte of the current prelude
    for (std::size_t i = 0; i < css.size(); ++i) {
        const char c = css[i];
        if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const std::size_t end = css.find("*/", i + 2);
            i = end == std::string_view::npos ? css.size() : end + 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            for (++i; i < css.size() && css[i] != c && css[i] != '\n'; ++i) {
                i += css[i] == '\\' ? 1 : 0;
            }
            continue;
        }
        if (c == '{') {
            stack.push_back(!at_rule);
            declaration_depth += at_rule ? 0 : 1;
            at_rule = false;
            seen = false;
        } else if (c == '}') {
            if (!stack.empty()) {
                declaration_depth -= stack.back() ? 1 : 0;
                stack.pop_back();
            }
            at_rule = false;
            seen = false;
        } else if (c == ';') {
            at_rule = false;
            seen = false;
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            if (!seen) {
                seen = true;
                at_rule = c == '@';
            }
            const bool selector = declaration_depth == 0 && !at_rule;
            if (c == '.' && selector && i + 1 < css.size() &&
                starts_name(static_cast<unsigned char>(css[i + 1]))) {
                std::size_t end = i + 1;
                while (end < css.size() && is_name_byte(static_cast<unsigned char>(css[end]))) {
                    ++end;
                }
                out.push_back({std::string(css.substr(i + 1, end - i - 1)),
                               map.to_utf16(static_cast<std::uint32_t>(i + 1)),
                               map.to_utf16(static_cast<std::uint32_t>(end))});
                i = end - 1;
            }
        }
    }
    return out;
}

} // namespace tessera::compiler::analysis
