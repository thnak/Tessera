// SPDX-License-Identifier: MIT

#include "paint_text_flex.hpp"
#include "tessera/ui/scene/text_wrap.hpp"

#include <string>

namespace tessera::ui {

namespace {

bool is(const style::ComputedStyle& style, const char* property, std::string_view keyword) {
    const std::string* value = style.find(property);
    return value != nullptr && *value == keyword;
}

} // namespace

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
