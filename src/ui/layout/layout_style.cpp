// SPDX-License-Identifier: MIT
//
// CSS declarations -> Yoga node style (split from layout_engine.cpp, which keeps tree building,
// text measurement and box collection).

#include "layout_style.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

namespace tessera::ui::layout::detail {

using style::ComputedStyle;

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trim(std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
                          return std::isspace(c) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool is_keyword(const std::string* value, std::string_view keyword) {
    return value != nullptr && lower(trim(*value)) == keyword;
}

bool parse_number(std::string_view text, float& value) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(token.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

namespace {

struct Length {
    enum class Kind : std::uint8_t {
        Invalid,
        Auto,
        Points,
        Percent,
    };

    Kind kind = Kind::Invalid;
    float value = 0.0f;
};

const std::string* property(const ComputedStyle& style, const char* name) {
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
        return Length{Length::Kind::Points, value * 16.0f};
    }
    if (unit == "vw") {
        return Length{Length::Kind::Points, value * viewport.width / 100.0f};
    }
    if (unit == "vh") {
        return Length{Length::Kind::Points, value * viewport.height / 100.0f};
    }
    if (unit == "pt") {
        return Length{Length::Kind::Points, value * 4.0f / 3.0f};
    }
    return {};
}

void apply_dimension(YGNodeRef node, const ComputedStyle& style, const char* name,
                     const LayoutViewport& viewport, void (*set_points)(YGNodeRef, float),
                     void (*set_percent)(YGNodeRef, float), void (*set_auto)(YGNodeRef)) {
    const std::string* value = property(style, name);
    if (value == nullptr) {
        return;
    }
    const Length length = parse_length(*value, viewport);
    switch (length.kind) {
    case Length::Kind::Points:
        set_points(node, length.value);
        break;
    case Length::Kind::Percent:
        set_percent(node, length.value);
        break;
    case Length::Kind::Auto:
        if (set_auto != nullptr) {
            set_auto(node);
        }
        break;
    case Length::Kind::Invalid:
        break;
    }
}

// Physical edges first, then the logical inline-axis edges. Yoga resolves `Start`/`End` against the
// node's own `direction` and prefers them over the physical edge they map to, so a declared
// logical longhand overrides that physical side and an undeclared one leaves it in effect
// (stylesheet-v1 section 11).
void apply_margin(YGNodeRef node, const ComputedStyle& style, const LayoutViewport& viewport) {
    constexpr const char* names[] = {"margin-top",  "margin-right",        "margin-bottom",
                                     "margin-left", "margin-inline-start", "margin-inline-end"};
    constexpr YGEdge edges[] = {YGEdgeTop,  YGEdgeRight, YGEdgeBottom,
                                YGEdgeLeft, YGEdgeStart, YGEdgeEnd};
    for (std::size_t i = 0; i < std::size(edges); ++i) {
        const std::string* value = property(style, names[i]);
        if (value == nullptr) {
            continue;
        }
        const Length length = parse_length(*value, viewport);
        switch (length.kind) {
        case Length::Kind::Points:
            YGNodeStyleSetMargin(node, edges[i], length.value);
            break;
        case Length::Kind::Percent:
            YGNodeStyleSetMarginPercent(node, edges[i], length.value);
            break;
        case Length::Kind::Auto:
            YGNodeStyleSetMarginAuto(node, edges[i]);
            break;
        case Length::Kind::Invalid:
            break;
        }
    }
}

void apply_padding(YGNodeRef node, const ComputedStyle& style, const LayoutViewport& viewport) {
    constexpr const char* names[] = {"padding-top",  "padding-right",        "padding-bottom",
                                     "padding-left", "padding-inline-start", "padding-inline-end"};
    constexpr YGEdge edges[] = {YGEdgeTop,  YGEdgeRight, YGEdgeBottom,
                                YGEdgeLeft, YGEdgeStart, YGEdgeEnd};
    for (std::size_t i = 0; i < std::size(edges); ++i) {
        const std::string* value = property(style, names[i]);
        if (value == nullptr) {
            continue;
        }
        const Length length = parse_length(*value, viewport);
        if (length.kind == Length::Kind::Points) {
            YGNodeStyleSetPadding(node, edges[i], length.value);
        } else if (length.kind == Length::Kind::Percent) {
            YGNodeStyleSetPaddingPercent(node, edges[i], length.value);
        }
    }
}

// Width of one inline-axis border side in px: the logical width/style longhands override the
// physical side they map to, and an absent logical longhand falls back to that physical side (the
// same precedence Yoga applies to Start/End). A side without a painting style contributes 0.
float inline_border_width(const ComputedStyle& style, const char* logical_width,
                          const char* logical_style, const char* physical_width,
                          const char* physical_style, const LayoutViewport& viewport) {
    const std::string* style_value = property(style, logical_style);
    if (style_value == nullptr) {
        style_value = property(style, physical_style);
    }
    if (!is_keyword(style_value, "solid") && !is_keyword(style_value, "dotted") &&
        !is_keyword(style_value, "dashed") && !is_keyword(style_value, "double") &&
        !is_keyword(style_value, "groove") && !is_keyword(style_value, "ridge") &&
        !is_keyword(style_value, "inset") && !is_keyword(style_value, "outset")) {
        return 0.0f;
    }
    const std::string* width_value = property(style, logical_width);
    if (width_value == nullptr) {
        width_value = property(style, physical_width);
    }
    if (width_value == nullptr) {
        return 0.0f;
    }
    const Length length = parse_length(*width_value, viewport);
    return length.kind == Length::Kind::Points ? length.value : 0.0f;
}

// `scrollbar-gutter: stable` reserves the overlay scrollbar width on the inline-end edge
// (docs/specs/scroll-and-gestures-v1.md section 6): it is a layout input, so the content box - and
// therefore every child - narrows by that width even when the overlay is not painted. The gutter is
// added as point padding, which keeps the authored inline size under `box-sizing: border-box` and
// grows it by the gutter under `content-box` (Yoga cannot reserve space out of a content-box
// width); a percentage inline-end padding keeps its percentage only.
void apply_scrollbar_gutter(YGNodeRef node, const ComputedStyle& style,
                            const LayoutViewport& viewport) {
    if (!is_keyword(property(style, "scrollbar-gutter"), "stable")) {
        return;
    }
    const auto is_scroll = [&style](const char* name) {
        return is_keyword(property(style, name), "scroll") ||
               is_keyword(property(style, name), "auto");
    };
    if (!is_scroll("overflow-x") && !is_scroll("overflow-y")) {
        return;
    }
    const bool rtl = is_keyword(property(style, "direction"), "rtl");
    const YGEdge edge = rtl ? YGEdgeLeft : YGEdgeRight;
    const std::string* inline_end = property(style, rtl ? "padding-left" : "padding-right");
    const Length length = inline_end == nullptr ? Length{} : parse_length(*inline_end, viewport);
    if (length.kind == Length::Kind::Percent) {
        return;
    }
    const float base = length.kind == Length::Kind::Points ? length.value : 0.0f;
    YGNodeStyleSetPadding(node, edge, base + kOverlayScrollbarWidth);
}

void apply_border(YGNodeRef node, const ComputedStyle& style, const LayoutViewport& viewport) {
    constexpr const char* widths[] = {"border-top-width", "border-right-width",
                                      "border-bottom-width", "border-left-width"};
    constexpr const char* styles[] = {"border-top-style", "border-right-style",
                                      "border-bottom-style", "border-left-style"};
    constexpr YGEdge edges[] = {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft};
    for (std::size_t i = 0; i < std::size(edges); ++i) {
        if (is_keyword(property(style, styles[i]), "none") ||
            is_keyword(property(style, styles[i]), "hidden")) {
            YGNodeStyleSetBorder(node, edges[i], 0.0f);
            continue;
        }
        const std::string* value = property(style, widths[i]);
        if (value == nullptr) {
            continue;
        }
        const Length length = parse_length(*value, viewport);
        if (length.kind == Length::Kind::Points) {
            YGNodeStyleSetBorder(node, edges[i], length.value);
        }
    }

    // A logical side only touches the Start/End edge when one of its longhands is declared; an
    // undeclared one leaves the physical Left/Right border applied above in charge.
    const bool rtl = is_keyword(property(style, "direction"), "rtl");
    const char* start_width = rtl ? "border-right-width" : "border-left-width";
    const char* start_style = rtl ? "border-right-style" : "border-left-style";
    const char* end_width = rtl ? "border-left-width" : "border-right-width";
    const char* end_style = rtl ? "border-left-style" : "border-right-style";
    if (property(style, "border-inline-start-width") != nullptr ||
        property(style, "border-inline-start-style") != nullptr) {
        YGNodeStyleSetBorder(node, YGEdgeStart,
                             inline_border_width(style, "border-inline-start-width",
                                                 "border-inline-start-style", start_width,
                                                 start_style, viewport));
    }
    if (property(style, "border-inline-end-width") != nullptr ||
        property(style, "border-inline-end-style") != nullptr) {
        YGNodeStyleSetBorder(node, YGEdgeEnd,
                             inline_border_width(style, "border-inline-end-width",
                                                 "border-inline-end-style", end_width, end_style,
                                                 viewport));
    }
}

void apply_position(YGNodeRef node, const ComputedStyle& style, const LayoutViewport& viewport) {
    constexpr const char* names[] = {
        "top", "right", "bottom", "left", "inset-inline-start", "inset-inline-end"};
    constexpr YGEdge edges[] = {YGEdgeTop,  YGEdgeRight, YGEdgeBottom,
                                YGEdgeLeft, YGEdgeStart, YGEdgeEnd};
    for (std::size_t i = 0; i < std::size(edges); ++i) {
        const std::string* value = property(style, names[i]);
        if (value == nullptr) {
            continue;
        }
        const Length length = parse_length(*value, viewport);
        switch (length.kind) {
        case Length::Kind::Points:
            YGNodeStyleSetPosition(node, edges[i], length.value);
            break;
        case Length::Kind::Percent:
            YGNodeStyleSetPositionPercent(node, edges[i], length.value);
            break;
        case Length::Kind::Auto:
            YGNodeStyleSetPositionAuto(node, edges[i]);
            break;
        case Length::Kind::Invalid:
            break;
        }
    }
}

YGAlign align_from(const std::string* value) {
    const std::string keyword = value == nullptr ? std::string() : lower(trim(*value));
    if (keyword == "flex-start" || keyword == "start" || keyword == "left") {
        return YGAlignFlexStart;
    }
    if (keyword == "center") {
        return YGAlignCenter;
    }
    if (keyword == "flex-end" || keyword == "end" || keyword == "right") {
        return YGAlignFlexEnd;
    }
    if (keyword == "baseline") {
        return YGAlignBaseline;
    }
    if (keyword == "space-between") {
        return YGAlignSpaceBetween;
    }
    if (keyword == "space-around") {
        return YGAlignSpaceAround;
    }
    if (keyword == "space-evenly") {
        return YGAlignSpaceEvenly;
    }
    if (keyword == "normal" || keyword == "stretch" || keyword == "auto") {
        return YGAlignStretch;
    }
    return YGAlignStretch;
}

YGAlign align_self_from(const std::string* value) {
    const std::string keyword = value == nullptr ? std::string() : lower(trim(*value));
    if (keyword.empty() || keyword == "auto") {
        return YGAlignAuto;
    }
    return align_from(value);
}

YGJustify justify_from(const std::string* value) {
    const std::string keyword = value == nullptr ? std::string() : lower(trim(*value));
    if (keyword == "center") {
        return YGJustifyCenter;
    }
    if (keyword == "flex-end" || keyword == "end" || keyword == "right") {
        return YGJustifyFlexEnd;
    }
    if (keyword == "space-between") {
        return YGJustifySpaceBetween;
    }
    if (keyword == "space-around") {
        return YGJustifySpaceAround;
    }
    if (keyword == "space-evenly") {
        return YGJustifySpaceEvenly;
    }
    return YGJustifyFlexStart;
}

YGFlexDirection flex_direction_from(const std::string* value) {
    const std::string keyword = value == nullptr ? std::string() : lower(trim(*value));
    if (keyword == "column") {
        return YGFlexDirectionColumn;
    }
    if (keyword == "column-reverse") {
        return YGFlexDirectionColumnReverse;
    }
    if (keyword == "row-reverse") {
        return YGFlexDirectionRowReverse;
    }
    return YGFlexDirectionRow;
}

} // namespace

void apply_style(YGNodeRef node, const ComputedStyle& style, const LayoutViewport& viewport) {
    const std::string* display = property(style, "display");
    const std::string display_value = display == nullptr ? std::string() : lower(trim(*display));
    if (display_value == "none") {
        YGNodeStyleSetDisplay(node, YGDisplayNone);
    } else if (display_value == "contents") {
        YGNodeStyleSetDisplay(node, YGDisplayContents);
    } else {
        YGNodeStyleSetDisplay(node, YGDisplayFlex);
        if (display_value == "block") {
            YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumn);
            YGNodeStyleSetAlignItems(node, YGAlignStretch);
        } else {
            YGNodeStyleSetFlexDirection(node,
                                        flex_direction_from(property(style, "flex-direction")));
        }
    }

    const std::string* wrap = property(style, "flex-wrap");
    if (is_keyword(wrap, "wrap")) {
        YGNodeStyleSetFlexWrap(node, YGWrapWrap);
    } else if (is_keyword(wrap, "wrap-reverse")) {
        YGNodeStyleSetFlexWrap(node, YGWrapWrapReverse);
    }

    YGNodeStyleSetJustifyContent(node, justify_from(property(style, "justify-content")));
    YGNodeStyleSetAlignItems(node, align_from(property(style, "align-items")));
    YGNodeStyleSetAlignContent(node, align_from(property(style, "align-content")));
    YGNodeStyleSetAlignSelf(node, align_self_from(property(style, "align-self")));

    const std::string* position = property(style, "position");
    if (is_keyword(position, "absolute") || is_keyword(position, "fixed")) {
        YGNodeStyleSetPositionType(node, YGPositionTypeAbsolute);
    } else if (is_keyword(position, "relative") || is_keyword(position, "sticky")) {
        YGNodeStyleSetPositionType(node, YGPositionTypeRelative);
    } else {
        YGNodeStyleSetPositionType(node, YGPositionTypeStatic);
    }

    if (is_keyword(property(style, "overflow-y"), "hidden") ||
        is_keyword(property(style, "overflow-y"), "clip")) {
        YGNodeStyleSetOverflow(node, YGOverflowHidden);
    } else if (is_keyword(property(style, "overflow-y"), "scroll") ||
               is_keyword(property(style, "overflow-y"), "auto")) {
        YGNodeStyleSetOverflow(node, YGOverflowScroll);
    } else if (is_keyword(property(style, "overflow-x"), "hidden") ||
               is_keyword(property(style, "overflow-x"), "clip")) {
        YGNodeStyleSetOverflow(node, YGOverflowHidden);
    } else if (is_keyword(property(style, "overflow-x"), "scroll") ||
               is_keyword(property(style, "overflow-x"), "auto")) {
        YGNodeStyleSetOverflow(node, YGOverflowScroll);
    }

    const std::string* direction = property(style, "direction");
    YGNodeStyleSetDirection(node, is_keyword(direction, "rtl") ? YGDirectionRTL : YGDirectionLTR);

    const std::string* box_sizing = property(style, "box-sizing");
    YGNodeStyleSetBoxSizing(node, is_keyword(box_sizing, "border-box") ? YGBoxSizingBorderBox
                                                                       : YGBoxSizingContentBox);

    apply_dimension(node, style, "width", viewport, YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent,
                    YGNodeStyleSetWidthAuto);
    apply_dimension(node, style, "height", viewport, YGNodeStyleSetHeight,
                    YGNodeStyleSetHeightPercent, YGNodeStyleSetHeightAuto);
    apply_dimension(node, style, "min-width", viewport, YGNodeStyleSetMinWidth,
                    YGNodeStyleSetMinWidthPercent, nullptr);
    apply_dimension(node, style, "min-height", viewport, YGNodeStyleSetMinHeight,
                    YGNodeStyleSetMinHeightPercent, nullptr);
    apply_dimension(node, style, "max-width", viewport, YGNodeStyleSetMaxWidth,
                    YGNodeStyleSetMaxWidthPercent, nullptr);
    apply_dimension(node, style, "max-height", viewport, YGNodeStyleSetMaxHeight,
                    YGNodeStyleSetMaxHeightPercent, nullptr);
    apply_dimension(node, style, "flex-basis", viewport, YGNodeStyleSetFlexBasis,
                    YGNodeStyleSetFlexBasisPercent, YGNodeStyleSetFlexBasisAuto);

    float number = 0.0f;
    if (property(style, "flex-grow") != nullptr &&
        parse_number(*property(style, "flex-grow"), number)) {
        YGNodeStyleSetFlexGrow(node, number);
    }
    if (property(style, "flex-shrink") != nullptr &&
        parse_number(*property(style, "flex-shrink"), number)) {
        YGNodeStyleSetFlexShrink(node, number);
    }

    const std::string* aspect_ratio = property(style, "aspect-ratio");
    // CSS applies the ratio only while one axis is `auto`; an explicit `width` and `height` both
    // win. Yoga's flex cross-size pass lets a ratio override a definite cross size, so the ratio is
    // not installed at all when both axes are already definite (issue #215).
    const auto is_defined_length = [](const Length& length) {
        return length.kind == Length::Kind::Points || length.kind == Length::Kind::Percent;
    };
    const bool both_axes_defined =
        property(style, "width") != nullptr && property(style, "height") != nullptr &&
        is_defined_length(parse_length(*property(style, "width"), viewport)) &&
        is_defined_length(parse_length(*property(style, "height"), viewport));
    if (aspect_ratio != nullptr && !both_axes_defined) {
        const std::string value = trim(*aspect_ratio);
        const std::size_t slash = value.find('/');
        float ratio = 0.0f;
        if (slash == std::string::npos) {
            if (parse_number(value, ratio) && ratio > 0.0f) {
                YGNodeStyleSetAspectRatio(node, ratio);
            }
        } else {
            float numerator = 0.0f;
            float denominator = 0.0f;
            if (parse_number(std::string_view(value).substr(0, slash), numerator) &&
                parse_number(std::string_view(value).substr(slash + 1), denominator) &&
                denominator > 0.0f) {
                YGNodeStyleSetAspectRatio(node, numerator / denominator);
            }
        }
    }

    apply_margin(node, style, viewport);
    apply_padding(node, style, viewport);
    apply_scrollbar_gutter(node, style, viewport);
    apply_border(node, style, viewport);
    apply_position(node, style, viewport);

    constexpr const char* gap_names[] = {"row-gap", "column-gap"};
    constexpr YGGutter gutters[] = {YGGutterRow, YGGutterColumn};
    for (std::size_t i = 0; i < 2; ++i) {
        const std::string* gap = property(style, gap_names[i]);
        if (gap == nullptr) {
            continue;
        }
        const Length length = parse_length(*gap, viewport);
        if (length.kind == Length::Kind::Points) {
            YGNodeStyleSetGap(node, gutters[i], length.value);
        } else if (length.kind == Length::Kind::Percent) {
            YGNodeStyleSetGapPercent(node, gutters[i], length.value);
        }
    }
}

} // namespace tessera::ui::layout::detail
