// SPDX-License-Identifier: MIT

#include "icon_svg.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::icons {
namespace {

[[nodiscard]] bool ascii_iequals(std::string_view text, std::string_view lower) noexcept {
    if (text.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != lower[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ascii_contains_iequals(std::string_view text, std::string_view lower) noexcept {
    if (lower.empty() || text.size() < lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i + lower.size() <= text.size(); ++i) {
        if (ascii_iequals(text.substr(i, lower.size()), lower)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Elements whose meaning the engine cannot represent, mirroring src/rendering/path/svg_prescan.cpp.
// `style` is added here: the engine ignores a stylesheet, so an icon that needs one would render as
// a different icon instead of failing.
[[nodiscard]] bool is_unsupported_tag(std::string_view name) noexcept {
    static constexpr std::string_view kUnsupported[] = {
        "text",
        "tspan",
        "textpath",
        "image",
        "foreignobject",
        "filter",
        "lineargradient",
        "radialgradient",
        "conicgradient",
        "pattern",
        "mask",
        "clippath",
        "animate",
        "animatetransform",
        "animatemotion",
        "set",
        "use",
        "script",
        "switch",
        "marker",
        "style",
        "font",
        "glyph",
        "symbol",
    };
    if (name.size() >= 2 && (name[0] == 'f') && (name[1] == 'e')) {
        return true; // filter primitive
    }
    for (const std::string_view unsupported : kUnsupported) {
        if (ascii_iequals(name, unsupported)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_paint_attribute(std::string_view name) noexcept {
    return ascii_iequals(name, "fill") || ascii_iequals(name, "stroke");
}

// `none`, `currentColor` and `url(...)` references are kept: the first two already resolve the way
// the icon wants, and a reference is rejected earlier by the subset scan.
[[nodiscard]] bool keeps_paint_value(std::string_view value) noexcept {
    return value.empty() || ascii_iequals(value, "none") || ascii_iequals(value, "currentcolor") ||
           ascii_iequals(value, "inherit") || ascii_contains_iequals(value, "url(");
}

// Rewrites the attributes of one tag body (the text between `<name` and `>`), preserving whitespace
// and quoting so the result stays byte-stable.
[[nodiscard]] std::string rewrite_tag(std::string_view tag, bool monochrome) {
    std::string out;
    out.reserve(tag.size());
    std::size_t index = 0;
    while (index < tag.size()) {
        const char c = tag[index];
        if (is_space(c) || c == '/' || c == '=') {
            out.push_back(c);
            ++index;
            continue;
        }
        const std::size_t name_start = index;
        while (index < tag.size() && !is_space(tag[index]) && tag[index] != '=' &&
               tag[index] != '/') {
            ++index;
        }
        const std::string_view name = tag.substr(name_start, index - name_start);
        out.append(name);
        const std::size_t after_name = index;
        while (index < tag.size() && is_space(tag[index])) {
            ++index;
        }
        if (index >= tag.size() || tag[index] != '=') {
            out.append(tag.substr(after_name, index - after_name));
            continue;
        }
        out.push_back('=');
        ++index;
        const std::size_t before_value = index;
        while (index < tag.size() && is_space(tag[index])) {
            ++index;
        }
        out.append(tag.substr(before_value, index - before_value));
        if (index >= tag.size()) {
            break;
        }
        const char quote = tag[index];
        if (quote != '"' && quote != '\'') {
            // Unquoted value: copy it verbatim; the loader handles the quoting.
            const std::size_t value_start = index;
            while (index < tag.size() && !is_space(tag[index]) && tag[index] != '>') {
                ++index;
            }
            out.append(tag.substr(value_start, index - value_start));
            continue;
        }
        const std::size_t value_start = index + 1;
        std::size_t value_end = value_start;
        while (value_end < tag.size() && tag[value_end] != quote) {
            ++value_end;
        }
        const std::string_view value = tag.substr(value_start, value_end - value_start);
        out.push_back(quote);
        if (monochrome && is_paint_attribute(name) && !keeps_paint_value(value)) {
            out.append("currentColor");
        } else {
            out.append(value);
        }
        if (value_end < tag.size()) {
            out.push_back(quote);
            index = value_end + 1;
        } else {
            index = value_end;
        }
    }
    return out;
}

} // namespace

std::expected<std::string, std::string> prepare_icon_body(std::string_view body, bool monochrome) {
    body = trim(body);
    if (body.empty()) {
        return std::unexpected("the icon body is empty");
    }
    std::string out;
    out.reserve(body.size());
    std::size_t index = 0;
    while (index < body.size()) {
        const char c = body[index];
        if (c != '<') {
            out.push_back(c);
            ++index;
            continue;
        }
        // Comments are dropped: they carry no drawing and their text would make the bytes depend on
        // the icon author's editing history.
        if (body.substr(index, 4) == "<!--") {
            const std::size_t end = body.find("-->", index + 4);
            if (end == std::string_view::npos) {
                return std::unexpected("the icon body has an unterminated comment");
            }
            index = end + 3;
            continue;
        }
        // A closing tag needs no attribute work.
        if (index + 1 < body.size() && body[index + 1] == '/') {
            const std::size_t end = body.find('>', index);
            if (end == std::string_view::npos) {
                return std::unexpected("the icon body has an unterminated closing tag");
            }
            out.append(body.substr(index, end - index + 1));
            index = end + 1;
            continue;
        }
        // Opening tag: read the element name, then the attribute body (quote-aware).
        std::size_t name_end = index + 1;
        while (name_end < body.size()) {
            const char name_char = body[name_end];
            if (is_space(name_char) || name_char == '>' || name_char == '/') {
                break;
            }
            ++name_end;
        }
        if (name_end == index + 1) {
            return std::unexpected("the icon body has a stray '<'");
        }
        const std::string_view name = body.substr(index + 1, name_end - index - 1);
        if (is_unsupported_tag(name)) {
            return std::unexpected("the icon body uses <" + std::string(name) +
                                   ">, which is outside the engine's static-icon subset");
        }
        std::size_t end = name_end;
        char quote = 0;
        while (end < body.size()) {
            const char tag_char = body[end];
            if (quote != 0) {
                if (tag_char == quote) {
                    quote = 0;
                }
            } else if (tag_char == '"' || tag_char == '\'') {
                quote = tag_char;
            } else if (tag_char == '>') {
                break;
            } else if (tag_char == '<') {
                return std::unexpected("the icon body has an unterminated <" + std::string(name) +
                                       "> tag");
            }
            ++end;
        }
        if (end >= body.size()) {
            return std::unexpected("the icon body has an unterminated <" + std::string(name) +
                                   "> tag");
        }
        const std::string_view tag_body = body.substr(name_end, end - name_end);
        if (ascii_contains_iequals(tag_body, "url(") || ascii_contains_iequals(tag_body, "href")) {
            return std::unexpected("the icon body references an external or internal resource "
                                   "(href/url(...)), which the static-icon subset does not allow");
        }
        for (const std::string_view unsupported : {"class=\"", "style=\"", "class='", "style='"}) {
            if (ascii_contains_iequals(tag_body, unsupported)) {
                return std::unexpected("the icon body uses a CSS class or style attribute, which "
                                       "needs a stylesheet the static-icon subset does not carry");
            }
        }
        out.append(body.substr(index, name_end - index));
        out.append(rewrite_tag(tag_body, monochrome));
        out.push_back('>');
        index = end + 1;
    }
    return out;
}

std::expected<std::string, std::string> canonical_icon_svg(const IconSource& icon,
                                                           const SvgOptions& options) {
    const auto body = prepare_icon_body(icon.body, options.monochrome);
    if (!body) {
        return std::unexpected(body.error());
    }
    const std::uint32_t left = icon.left;
    const std::uint32_t top = icon.top;
    const std::uint32_t width = icon.width == 0 ? 24 : icon.width;
    const std::uint32_t height = icon.height == 0 ? 24 : icon.height;
    std::string svg;
    svg.reserve(body->size() + 160);
    svg.append("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"");
    svg.append(std::to_string(width));
    svg.append("\" height=\"");
    svg.append(std::to_string(height));
    svg.append("\" viewBox=\"");
    svg.append(std::to_string(left));
    svg.push_back(' ');
    svg.append(std::to_string(top));
    svg.push_back(' ');
    svg.append(std::to_string(width));
    svg.push_back(' ');
    svg.append(std::to_string(height));
    svg.append("\">");
    svg.append(*body);
    svg.append("</svg>\n");
    return svg;
}

} // namespace tessera::icons
