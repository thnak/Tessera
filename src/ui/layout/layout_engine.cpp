// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yoga/Yoga.h>

namespace tessera::ui::layout {
namespace {

using style::ComputedStyle;

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

const std::string* property(const ComputedStyle& style, const char* name) {
    return style.find(name);
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

void apply_margin(YGNodeRef node, const ComputedStyle& style, const LayoutViewport& viewport) {
    constexpr const char* names[] = {"margin-top", "margin-right", "margin-bottom", "margin-left"};
    constexpr YGEdge edges[] = {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft};
    for (std::size_t i = 0; i < 4; ++i) {
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
    constexpr const char* names[] = {"padding-top", "padding-right", "padding-bottom",
                                     "padding-left"};
    constexpr YGEdge edges[] = {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft};
    for (std::size_t i = 0; i < 4; ++i) {
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

void apply_border(YGNodeRef node, const ComputedStyle& style, const LayoutViewport& viewport) {
    constexpr const char* widths[] = {"border-top-width", "border-right-width",
                                      "border-bottom-width", "border-left-width"};
    constexpr const char* styles[] = {"border-top-style", "border-right-style",
                                      "border-bottom-style", "border-left-style"};
    constexpr YGEdge edges[] = {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft};
    for (std::size_t i = 0; i < 4; ++i) {
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
}

void apply_position(YGNodeRef node, const ComputedStyle& style, const LayoutViewport& viewport) {
    constexpr const char* names[] = {"top", "right", "bottom", "left"};
    constexpr YGEdge edges[] = {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft};
    for (std::size_t i = 0; i < 4; ++i) {
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
    if (aspect_ratio != nullptr) {
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

struct MeasureContext {
    const LayoutInput* input = nullptr;
    text::TextStyle style;
    text::TextMeasureCache* cache = nullptr;
};

YGSize measure_text(YGNodeConstRef node, float width, YGMeasureMode width_mode, float height,
                    YGMeasureMode height_mode) {
    auto* context = static_cast<MeasureContext*>(YGNodeGetContext(node));
    if (context == nullptr || context->cache == nullptr) {
        return YGSize{0.0f, 0.0f};
    }

    text::TextConstraints constraints;
    constraints.max_width =
        width_mode == YGMeasureModeUndefined ? std::numeric_limits<float>::infinity() : width;
    constraints.wrap = width_mode != YGMeasureModeUndefined;
    const text::TextMetrics metrics =
        context->cache->measure(context->input->text, context->style, constraints);

    const float measured_width = width_mode == YGMeasureModeExactly ? width : metrics.width;
    const float measured_height = height_mode == YGMeasureModeExactly ? height : metrics.height;
    return YGSize{measured_width, measured_height};
}

YGNodeRef build_node(const LayoutInput& input, const LayoutViewport& viewport,
                     text::TextMeasureCache& cache,
                     std::vector<std::unique_ptr<MeasureContext>>& contexts) {
    YGNodeRef node = YGNodeNew();
    apply_style(node, input.style, viewport);
    if (!input.text.empty() && input.children.empty()) {
        contexts.push_back(std::make_unique<MeasureContext>(
            MeasureContext{&input, text::text_style_from(input.style), &cache}));
        YGNodeSetContext(node, contexts.back().get());
        YGNodeSetMeasureFunc(node, measure_text);
    }
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        YGNodeInsertChild(node, build_node(input.children[i], viewport, cache, contexts), i);
    }
    return node;
}

void collect_boxes(YGNodeRef node, const LayoutInput& input, float parent_x, float parent_y,
                   LayoutResult& result) {
    const float left = YGNodeLayoutGetLeft(node);
    const float top = YGNodeLayoutGetTop(node);
    const float x = parent_x + left;
    const float y = parent_y + top;
    result.boxes.push_back(LayoutBox{
        input.id, rhi::Rect{x, y, YGNodeLayoutGetWidth(node), YGNodeLayoutGetHeight(node)}});

    for (std::size_t i = 0; i < input.children.size(); ++i) {
        collect_boxes(YGNodeGetChild(node, i), input.children[i], x, y, result);
    }
}

YGDirection root_direction(const LayoutInput& root) {
    const std::string* direction = root.style.find("direction");
    return is_keyword(direction, "rtl") ? YGDirectionRTL : YGDirectionLTR;
}

} // namespace

struct LayoutEngine::Impl {
    Impl(std::shared_ptr<const text::TextMeasurer> text_measurer, std::size_t cache_capacity)
        : cache(
              std::make_shared<text::TextMeasureCache>(std::move(text_measurer), cache_capacity)) {}

    std::shared_ptr<text::TextMeasureCache> cache;
};

LayoutEngine::LayoutEngine(std::shared_ptr<const text::TextMeasurer> measurer,
                           std::size_t text_cache_capacity) {
    if (!measurer) {
        measurer = std::make_shared<text::StubTextMeasurer>();
    }
    impl_ = std::make_unique<Impl>(std::move(measurer), text_cache_capacity);
}

LayoutEngine::~LayoutEngine() = default;
LayoutEngine::LayoutEngine(LayoutEngine&&) noexcept = default;
LayoutEngine& LayoutEngine::operator=(LayoutEngine&&) noexcept = default;

LayoutResult LayoutEngine::layout(const LayoutInput& root, const LayoutViewport& viewport) const {
    std::vector<std::unique_ptr<MeasureContext>> contexts;
    YGNodeRef root_node = build_node(root, viewport, *impl_->cache, contexts);
    YGNodeCalculateLayout(root_node, viewport.width, viewport.height, root_direction(root));

    LayoutResult result;
    result.boxes.reserve(16);
    collect_boxes(root_node, root, 0.0f, 0.0f, result);
    YGNodeFreeRecursive(root_node);
    return result;
}

std::shared_ptr<text::TextMeasureCache> LayoutEngine::text_cache() const {
    return impl_->cache;
}

} // namespace tessera::ui::layout
