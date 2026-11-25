// SPDX-License-Identifier: MIT
//
// See layout_length.hpp for why this is its own unit.

#include "layout_length.hpp"
#include "layout_style.hpp"
#include "tessera/ui/style/viewport_units.hpp"

#include <cctype>

namespace tessera::ui::layout {

using detail::lower;
using detail::parse_number;
using detail::trim;
using style::ComputedStyle;

const std::string* computed_property(const ComputedStyle& style, const char* name) {
    return style.find(name);
}

Length parse_length(std::string_view text, const LayoutViewport& viewport) {
    const std::string token = lower(trim(text));
    if (token == "auto") {
        return Length{Length::Kind::Auto, 0.0f};
    }
    if (token.empty()) {
        return {};
    }

    if (token.back() == '%') {
        float value = 0.0f;
        if (!parse_number(std::string_view(token).substr(0, token.size() - 1), value)) {
            return {};
        }
        return Length{Length::Kind::Percent, value};
    }

    std::size_t unit_start = 0;
    while (unit_start < token.size() &&
           (std::isdigit(static_cast<unsigned char>(token[unit_start])) != 0 ||
            token[unit_start] == '.' || token[unit_start] == '+' || token[unit_start] == '-')) {
        ++unit_start;
    }
    const std::string_view unit(token.data() + unit_start, token.size() - unit_start);
    float value = 0.0f;
    if (!parse_number(std::string_view(token).substr(0, unit_start), value)) {
        return {};
    }

    if (unit.empty() || unit == "px") {
        return Length{Length::Kind::Points, value};
    }
    if (unit == "rem" || unit == "em") {
        return Length{Length::Kind::Points, value * (unit == "em" ? viewport.font_size : 16.0f)};
    }
    // Every viewport unit comes from the one shared table (issue #691), so layout, grid and
    // `@media` accept exactly the same set - including `vmin`, `vmax` and the small/large/dynamic
    // family, which used to fail to parse here even though the spec lists them as V1.
    style::ViewportSize size = style::ViewportSize::Default;
    style::ViewportAxis axis = style::ViewportAxis::Width;
    if (style::parse_viewport_unit(unit, size, axis)) {
        style::ViewportUnits units;
        units.width = viewport.width;
        units.height = viewport.height;
        units.font_size = viewport.font_size;
        return Length{Length::Kind::Points, static_cast<float>(style::resolve_viewport_length(
                                                style::ViewportLength{size, axis, value}, units))};
    }
    if (unit == "pt") {
        return Length{Length::Kind::Points, value * 4.0f / 3.0f};
    }
    return {};
}

} // namespace tessera::ui::layout