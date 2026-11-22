// SPDX-License-Identifier: MIT
//
// The measure seam of the layout engine: Yoga's measure math, the grid item layouter, the CSS
// `order` permutation and the text-fit fact. Shared by the cold tree builder (layout_cold.cpp) and
// the retained tree (layout_retained.cpp), so both paths measure with the very same arithmetic.

#include "layout_cold.hpp"
#include "layout_measure.hpp"
#include "layout_style.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>

namespace tessera::ui::layout::detail {

using style::ComputedStyle;

YGConfigRef unrounded_config() {
    // Unrounded layout: Yoga's own pixel rounding floors a measured node's left edge and ceils its
    // right one, which moved a centred glyph by most of a pixel. Positions stay fractional and
    // paint snaps them (pixel_snap.cpp), so a centred mark is centred before it is snapped.
    static YGConfigRef config = [] {
        YGConfigRef created = YGConfigNew();
        YGConfigSetPointScaleFactor(created, 0.0f);
        return created;
    }();
    return config;
}

bool is_grid_container(const ComputedStyle& style) {
    return is_keyword(style.find("display"), "grid") ||
           is_keyword(style.find("display"), "inline-grid");
}

int order_value(const ComputedStyle& style) {
    const std::string* value = style.find("order");
    if (value == nullptr) {
        return 0;
    }
    // `parse_number` trims and rejects a non-finite token.
    float number = 0.0f;
    if (!parse_number(*value, number)) {
        return 0;
    }
    // CSS `order` is an `<integer>`: a fractional or out-of-range value is invalid and falls back
    // to the initial `0` rather than silently truncating.
    if (number != std::floor(number) || number < -1.0e6f || number > 1.0e6f) {
        return 0;
    }
    return static_cast<int>(number);
}

bool child_layout_order(const LayoutInput& input, std::vector<std::size_t>& order,
                        std::vector<int>& keys) {
    order.clear();
    bool permuted = false;
    for (const LayoutInput& child : input.children) {
        if (order_value(child.style) != 0) {
            permuted = true;
            break;
        }
    }
    if (!permuted) {
        return false;
    }
    order.resize(input.children.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    // The key is read once per child, not once per comparison: a computed-style lookup is a map
    // search, and a stable sort compares O(n log n) times.
    keys.resize(input.children.size());
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        keys[i] = order_value(input.children[i].style);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&keys](std::size_t lhs, std::size_t rhs) { return keys[lhs] < keys[rhs]; });
    return true;
}

float paint_padding_px(const ComputedStyle& style, const char* property) {
    const std::string* value = style.find(property);
    if (value == nullptr) {
        return 0.0f;
    }
    std::string token = trim(*value);
    if (token.empty()) {
        return 0.0f;
    }
    if (token.size() > 2 && token.compare(token.size() - 2, 2, "px") == 0) {
        token = trim(std::string_view(token).substr(0, token.size() - 2));
    } else if (std::isalpha(static_cast<unsigned char>(token.back())) != 0 || token.back() == '%') {
        return 0.0f;
    }
    float number = 0.0f;
    if (!parse_number(token, number) || number < 0.0f) {
        return 0.0f;
    }
    return number;
}

TextFitInputs text_fit_inputs(const LayoutInput& input, const text::TextStyle& style) {
    TextFitInputs inputs;
    inputs.text = input.text;
    inputs.style = &style;
    inputs.text_wrap = input.text_wrap;
    inputs.display_none = is_keyword(input.style.find("display"), "none");
    inputs.pad_left = paint_padding_px(input.style, "padding-left");
    inputs.pad_top = paint_padding_px(input.style, "padding-top");
    inputs.pad_right = paint_padding_px(input.style, "padding-right");
    inputs.pad_bottom = paint_padding_px(input.style, "padding-bottom");
    return inputs;
}

TextFit measure_text_fit(const TextFitInputs& inputs, const rhi::Rect& bounds,
                         text::TextMeasureCache& cache) {
    if (inputs.text.empty() || inputs.display_none || inputs.style == nullptr) {
        return TextFit::Unknown;
    }
    const text::TextStyle& text_style = *inputs.style;
    const float content_width = std::max(0.0f, bounds.width - inputs.pad_left - inputs.pad_right);
    const float content_height = std::max(0.0f, bounds.height - inputs.pad_top - inputs.pad_bottom);
    // Paint returns before emitting anything when its content box is empty, so a degenerate box is
    // bounded - but it is not a fact worth proving, and `Unknown` keeps the safest answer.
    if (content_width <= 0.0f || content_height <= 0.0f) {
        return TextFit::Unknown;
    }

    text::TextConstraints constraints;
    constraints.max_width = content_width;
    constraints.wrap = inputs.text_wrap && text::white_space_wraps(text_style.white_space);
    const text::TextMetrics metrics = cache.measure(inputs.text, text_style, constraints);
    if (metrics.width > content_width || metrics.height > content_height) {
        return TextFit::Overflows;
    }

    // A `line-height` shorter than the font's own line box makes paint's half-leading place glyph
    // ink above and below the line box it reports, so the measured height would understate what
    // paint emits. `line-height: normal` (0 here) is the font's own box and keeps the ink inside
    // it, and an explicit value at least one pixel taller is safe too: the wrap rounds the ascent
    // and descent before deriving the baseline, which can move it by half a pixel either way, and
    // the one pixel covers that. Anything tighter (or not provably looser) stays `Unknown`.
    if (text_style.line_height > 0.0f) {
        text::TextStyle natural = text_style;
        natural.line_height = 0.0f;
        const text::TextMetrics natural_metrics = cache.measure(inputs.text, natural, constraints);
        const float natural_line =
            natural_metrics.line_count > 0
                ? natural_metrics.height / static_cast<float>(natural_metrics.line_count)
                : 0.0f;
        if (text_style.line_height < natural_line + 1.0f) {
            return TextFit::Unknown;
        }
    }
    return TextFit::Fits;
}

TextFit measure_text_fit(const LayoutInput& input, const rhi::Rect& bounds,
                         text::TextMeasureCache& cache) {
    if (input.text.empty()) {
        return TextFit::Unknown;
    }
    const text::TextStyle style = text::text_style_from(input.style);
    return measure_text_fit(text_fit_inputs(input, style), bounds, cache);
}

void emit_zero_boxes(const LayoutInput& input, float x, float y, LayoutResult& result) {
    result.boxes.push_back(LayoutBox{input.id, rhi::Rect{x, y, 0.0f, 0.0f}, LayoutInsets{}});
    for (const LayoutInput& child : input.children) {
        emit_zero_boxes(child, x, y, result);
    }
}

YGSize measure_text_box(std::string_view text, const text::TextStyle& style,
                        text::TextMeasureCache& cache, float width, YGMeasureMode width_mode,
                        float height, YGMeasureMode height_mode) {
    text::TextConstraints constraints;
    constraints.max_width =
        width_mode == YGMeasureModeUndefined ? std::numeric_limits<float>::infinity() : width;
    constraints.wrap = width_mode != YGMeasureModeUndefined;
    // An empty `field-sizing: content` field is one line tall and no wider than its padding.
    const bool empty = text.empty();
    const text::TextMetrics metrics =
        cache.measure(empty ? std::string_view(" ") : text, style, constraints);

    const float measured_width =
        width_mode == YGMeasureModeExactly ? width : (empty ? 0.0f : metrics.width);
    const float measured_height = height_mode == YGMeasureModeExactly ? height : metrics.height;
    return YGSize{measured_width, measured_height};
}

YGSize measure_grid_box(const LayoutInput& container, const LayoutViewport& viewport,
                        text::TextMeasureCache& cache, float width, YGMeasureMode width_mode,
                        float height, YGMeasureMode height_mode) {
    // Only an exact constraint makes the container size definite. `AtMost` is Yoga's fit-content
    // probe: the grid then sizes to its content and is clamped to the probe, so a `fr` track does
    // not claim space the container has not been given and `auto` tracks do not stretch.
    GridContentBox content;
    content.width_definite = width_mode == YGMeasureModeExactly;
    content.width = content.width_definite ? std::max(0.0f, width) : 0.0f;
    content.height_definite = height_mode == YGMeasureModeExactly;
    content.height = content.height_definite ? std::max(0.0f, height) : 0.0f;
    float out_width = 0.0f;
    float out_height = 0.0f;
    run_grid(container, viewport, content, cache, 0.0f, 0.0f, nullptr, out_width, out_height);
    if (width_mode == YGMeasureModeAtMost) {
        out_width = std::min(out_width, std::max(0.0f, width));
    }
    if (height_mode == YGMeasureModeAtMost) {
        out_height = std::min(out_height, std::max(0.0f, height));
    }
    return YGSize{out_width, out_height};
}

GridItemLayout layout_grid_item(void* raw, const LayoutInput& item, float available_width,
                                float available_height, std::vector<LayoutBox>* boxes) {
    auto* context = static_cast<GridItemContext*>(raw);
    const LayoutResult result =
        layout_cold(item, *context->viewport, available_width, available_height, *context->cache,
                    /*as_grid_item=*/true);
    if (boxes != nullptr) {
        boxes->insert(boxes->end(), result.boxes.begin(), result.boxes.end());
    }
    GridItemLayout layout;
    if (!result.boxes.empty()) {
        layout.width = result.boxes.front().bounds.width;
        layout.height = result.boxes.front().bounds.height;
    }
    return layout;
}

void run_grid(const LayoutInput& container, const LayoutViewport& viewport,
              const GridContentBox& content, text::TextMeasureCache& cache, float origin_x,
              float origin_y, std::vector<LayoutBox>* boxes, float& out_width, float& out_height) {
    GridItemContext item_context{&viewport, &cache};
    const GridItemLayouter layouter{&layout_grid_item, &item_context};
    GridEngine engine(container, viewport, layouter);
    engine.run(content);
    out_width = engine.content_width();
    out_height = engine.content_height();
    if (boxes != nullptr) {
        engine.emit_boxes(origin_x, origin_y, *boxes);
    }
}

} // namespace tessera::ui::layout::detail
