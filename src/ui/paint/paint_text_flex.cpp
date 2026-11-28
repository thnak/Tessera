// SPDX-License-Identifier: MIT

#include "paint_text_flex.hpp"
#include "tessera/ui/scene/text_wrap.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace tessera::ui {

namespace {

bool is(const style::ComputedStyle& style, const char* property, std::string_view keyword) {
    const std::string* value = style.find(property);
    return value != nullptr && *value == keyword;
}

// Trimmed, lower-cased value of a computed property; empty when the property is absent.
std::string keyword_value(const style::ComputedStyle& style, const char* property) {
    const std::string* value = style.find(property);
    if (value == nullptr) {
        return {};
    }
    std::string token(*value);
    std::size_t first = 0;
    while (first < token.size() && std::isspace(static_cast<unsigned char>(token[first])) != 0) {
        ++first;
    }
    std::size_t last = token.size();
    while (last > first && std::isspace(static_cast<unsigned char>(token[last - 1])) != 0) {
        --last;
    }
    token = token.substr(first, last - first);
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return token;
}

} // namespace

// `start`/`end` are logical keywords: the inline start is the right edge under RTL, so the same
// stylesheet mirrors its text between LTR and RTL (stylesheet-v1 section 11). `left`/`right` stay
// physical; an unknown value (including the unimplemented `justify`) falls back to the logical
// start, as before.
PaintTextAlign computed_text_align(const style::ComputedStyle& style) {
    const std::string token = keyword_value(style, "text-align");
    if (token == "center") {
        return PaintTextAlign::Center;
    }
    if (token == "right") {
        return PaintTextAlign::End;
    }
    if (token == "left") {
        return PaintTextAlign::Start;
    }
    const bool rtl = keyword_value(style, "direction") == "rtl";
    const bool physical_end = (token == "end") != rtl;
    return physical_end ? PaintTextAlign::End : PaintTextAlign::Start;
}

FlexTextCentering flex_text_centering(std::string_view element_kind,
                                      const style::ComputedStyle& style) {
    FlexTextCentering result;
    if (element_kind != "Text" ||
        !(is(style, "display", "flex") || is(style, "display", "inline-flex"))) {
        return result;
    }
    const bool column =
        is(style, "flex-direction", "column") || is(style, "flex-direction", "column-reverse");
    const bool justify = is(style, "justify-content", "center");
    const bool align = is(style, "align-items", "center");
    result.horizontal = column ? align : justify;
    result.vertical = column ? justify : align;
    return result;
}

bool text_wraps(std::string_view element_kind, const PaintData& data, text::WhiteSpace white_space,
                const paint::PaintTextMetricsSource& metrics, const text::TextStyle& style,
                float content_height) {
    if (!paint_wraps(element_kind, data) || !text::white_space_wraps(white_space)) {
        return false;
    }
    if (data.wrap == PaintWrap::Wrap) {
        return true;
    }
    text::TextConstraints one_line;
    one_line.wrap = false;
    const text::TextLayout probe = metrics.layout("x", style, one_line);
    return probe.lines.empty() || content_height >= probe.lines.front().height * 1.5f;
}

} // namespace tessera::ui
