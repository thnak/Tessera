// SPDX-License-Identifier: MIT
//
// The layout seam: `LayoutEngine::layout` turns a `LayoutInput` tree into absolute boxes.
//
// Flexbox and block layout are delegated to the vendored Yoga (v3.2.2). CSS Grid has no Yoga
// equivalent, so a `display: grid` container becomes a Yoga leaf with a measure callback that
// reports the grid's content size; its items are laid out by `detail::GridEngine` after Yoga has
// resolved the container's content box (docs/specs/grid-v1.md).

#include "grid_engine.hpp"
#include "layout_style.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yoga/Yoga.h>

namespace tessera::ui::layout {
namespace {

using detail::apply_style;
using detail::is_keyword;
using detail::lower;
using detail::parse_number;
using detail::trim;
using style::ComputedStyle;

struct MeasureContext {
    const LayoutInput* input = nullptr;
    text::TextStyle style;
    text::TextMeasureCache* cache = nullptr;
};

struct GridContext {
    const LayoutInput* input = nullptr;
    const LayoutViewport* viewport = nullptr;
    text::TextMeasureCache* cache = nullptr;
    bool measure_none = false;
};

struct LayoutContext {
    const LayoutViewport* viewport = nullptr;
    text::TextMeasureCache* cache = nullptr;
};

struct GridItemContext {
    const LayoutViewport* viewport = nullptr;
    text::TextMeasureCache* cache = nullptr;
};

bool is_grid_container(const ComputedStyle& style) {
    return is_keyword(style.find("display"), "grid") ||
           is_keyword(style.find("display"), "inline-grid");
}

// Lays out one subtree as a whole tree. `as_grid_item` zeroes the root's margins because the grid
// engine positions the item's border box and folds the margins into the track sizes.
LayoutResult layout_tree(const LayoutInput& root, const LayoutViewport& viewport,
                         float available_width, float available_height,
                         text::TextMeasureCache& cache, bool as_grid_item);

YGSize measure_grid(YGNodeConstRef node, float width, YGMeasureMode width_mode, float height,
                    YGMeasureMode height_mode);
detail::GridItemLayout layout_grid_item(void* raw, const LayoutInput& item, float available_width,
                                        float available_height, std::vector<LayoutBox>* boxes);

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
    // An empty `field-sizing: content` field is one line tall and no wider than its padding.
    const bool empty = context->input->text.empty();
    const text::TextMetrics metrics = context->cache->measure(
        empty ? std::string_view(" ") : std::string_view(context->input->text), context->style,
        constraints);

    const float measured_width =
        width_mode == YGMeasureModeExactly ? width : (empty ? 0.0f : metrics.width);
    const float measured_height = height_mode == YGMeasureModeExactly ? height : metrics.height;
    return YGSize{measured_width, measured_height};
}

// CSS `order` (stylesheet-v1 section 11, issue #213): flex items are laid out in ascending `order`,
// ties keeping source order. Yoga has no `order` style, so the adapter inserts the children in the
// resolved order and walks them back in that same order when collecting boxes. `order` applies to
// every container here because the adapter maps `display: block` to a column flex container, so
// there is no non-flex parent to exempt.
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

// Fills `order` with the child indices in layout order and returns true, or returns false (leaving
// `order` empty) when every child is at the initial `0` — the common case, which needs no
// permutation and no allocation.
bool child_layout_order(const LayoutInput& input, std::vector<std::size_t>& order) {
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
    std::vector<int> keys(input.children.size());
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        keys[i] = order_value(input.children[i].style);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&keys](std::size_t lhs, std::size_t rhs) { return keys[lhs] < keys[rhs]; });
    return true;
}

YGNodeRef build_tree(const LayoutInput& input, const LayoutViewport& viewport,
                     text::TextMeasureCache& cache,
                     std::vector<std::unique_ptr<MeasureContext>>& measure_contexts,
                     std::vector<std::unique_ptr<GridContext>>& grid_contexts, bool as_grid_item) {
    YGNodeRef node = YGNodeNew();
    apply_style(node, input.style, viewport);
    if (as_grid_item) {
        // The grid engine already positions the item's border box and folds the margins into the
        // track sizes, so a root that re-applied them would double-count.
        for (const YGEdge edge : {YGEdgeLeft, YGEdgeTop, YGEdgeRight, YGEdgeBottom}) {
            YGNodeStyleSetMargin(node, edge, 0.0f);
        }
    }
    if (is_grid_container(input.style)) {
        auto context = std::make_unique<GridContext>();
        context->input = &input;
        context->viewport = &viewport;
        context->cache = &cache;
        context->measure_none = is_keyword(input.style.find("display"), "none");
        YGNodeSetContext(node, context.get());
        YGNodeSetMeasureFunc(node, measure_grid);
        grid_contexts.push_back(std::move(context));
        return node;
    }
    // `field-sizing: content` makes an editable field a measured leaf even when it is empty.
    const bool sizes_to_content = is_keyword(input.style.find("field-sizing"), "content");
    if ((!input.text.empty() || sizes_to_content) && input.children.empty()) {
        measure_contexts.push_back(std::make_unique<MeasureContext>(
            MeasureContext{&input, text::text_style_from(input.style), &cache}));
        YGNodeSetContext(node, measure_contexts.back().get());
        YGNodeSetMeasureFunc(node, measure_text);
    }
    std::vector<std::size_t> order;
    const bool permuted = child_layout_order(input, order);
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        const std::size_t index = permuted ? order[i] : i;
        YGNodeInsertChild(node,
                          build_tree(input.children[index], viewport, cache, measure_contexts,
                                     grid_contexts, false),
                          i);
    }
    return node;
}

YGDirection root_direction(const LayoutInput& root) {
    const std::string* direction = root.style.find("direction");
    return is_keyword(direction, "rtl") ? YGDirectionRTL : YGDirectionLTR;
}

void run_grid(const LayoutInput& container, const LayoutViewport& viewport,
              const detail::GridContentBox& content, text::TextMeasureCache& cache, float origin_x,
              float origin_y, std::vector<LayoutBox>* boxes, float& out_width, float& out_height) {
    GridItemContext item_context{&viewport, &cache};
    const detail::GridItemLayouter layouter{&layout_grid_item, &item_context};
    detail::GridEngine engine(container, viewport, layouter);
    engine.run(content);
    out_width = engine.content_width();
    out_height = engine.content_height();
    if (boxes != nullptr) {
        engine.emit_boxes(origin_x, origin_y, *boxes);
    }
}

YGSize measure_grid(YGNodeConstRef node, float width, YGMeasureMode width_mode, float height,
                    YGMeasureMode height_mode) {
    auto* context = static_cast<GridContext*>(YGNodeGetContext(node));
    if (context == nullptr) {
        return YGSize{0.0f, 0.0f};
    }
    // Only an exact constraint makes the container size definite. `AtMost` is Yoga's fit-content
    // probe: the grid then sizes to its content and is clamped to the probe, so a `fr` track does
    // not claim space the container has not been given and `auto` tracks do not stretch.
    detail::GridContentBox content;
    content.width_definite = width_mode == YGMeasureModeExactly;
    content.width = content.width_definite ? std::max(0.0f, width) : 0.0f;
    content.height_definite = height_mode == YGMeasureModeExactly;
    content.height = content.height_definite ? std::max(0.0f, height) : 0.0f;
    float out_width = 0.0f;
    float out_height = 0.0f;
    run_grid(*context->input, *context->viewport, content, *context->cache, 0.0f, 0.0f, nullptr,
             out_width, out_height);
    if (width_mode == YGMeasureModeAtMost) {
        out_width = std::min(out_width, std::max(0.0f, width));
    }
    if (height_mode == YGMeasureModeAtMost) {
        out_height = std::min(out_height, std::max(0.0f, height));
    }
    return YGSize{out_width, out_height};
}

// Lays out one grid item subtree for `GridEngine`. The item's border box is the subtree root, so
// the returned size is the size the engine positions and aligns.
detail::GridItemLayout layout_grid_item(void* raw, const LayoutInput& item, float available_width,
                                        float available_height, std::vector<LayoutBox>* boxes) {
    auto* context = static_cast<GridItemContext*>(raw);
    const LayoutResult result =
        layout_tree(item, *context->viewport, available_width, available_height, *context->cache,
                    /*as_grid_item=*/true);
    if (boxes != nullptr) {
        boxes->insert(boxes->end(), result.boxes.begin(), result.boxes.end());
    }
    detail::GridItemLayout layout;
    if (!result.boxes.empty()) {
        layout.width = result.boxes.front().bounds.width;
        layout.height = result.boxes.front().bounds.height;
    }
    return layout;
}

// `display: none` takes the whole subtree out of layout, matching the flex path: every descendant
// reports a zero box at the container's origin.
void emit_zero_boxes(const LayoutInput& input, float x, float y, LayoutResult& result) {
    result.boxes.push_back(LayoutBox{input.id, rhi::Rect{x, y, 0.0f, 0.0f}, LayoutInsets{}});
    for (const LayoutInput& child : input.children) {
        emit_zero_boxes(child, x, y, result);
    }
}

void collect_grid_items(YGNodeRef node, const LayoutInput& input, float x, float y,
                        const LayoutContext& context, LayoutResult& result) {
    const auto inset = [node](YGEdge edge) {
        return YGNodeLayoutGetPadding(node, edge) + YGNodeLayoutGetBorder(node, edge);
    };
    const float left = inset(YGEdgeLeft);
    const float top = inset(YGEdgeTop);
    if (is_keyword(input.style.find("display"), "none")) {
        for (const LayoutInput& child : input.children) {
            emit_zero_boxes(child, x + left, y + top, result);
        }
        return;
    }
    detail::GridContentBox content;
    content.width = std::max(0.0f, YGNodeLayoutGetWidth(node) - left - inset(YGEdgeRight));
    content.height = std::max(0.0f, YGNodeLayoutGetHeight(node) - top - inset(YGEdgeBottom));
    content.width_definite = true;
    content.height_definite = true;
    float out_width = 0.0f;
    float out_height = 0.0f;
    run_grid(input, *context.viewport, content, *context.cache, x + left, y + top, &result.boxes,
             out_width, out_height);
}

void collect_boxes(YGNodeRef node, const LayoutInput& input, float parent_x, float parent_y,
                   const LayoutContext& context, LayoutResult& result) {
    const float left = YGNodeLayoutGetLeft(node);
    const float top = YGNodeLayoutGetTop(node);
    const float x = parent_x + left;
    const float y = parent_y + top;
    const auto inset = [node](YGEdge edge) {
        return YGNodeLayoutGetPadding(node, edge) + YGNodeLayoutGetBorder(node, edge);
    };
    result.boxes.push_back(LayoutBox{
        input.id, rhi::Rect{x, y, YGNodeLayoutGetWidth(node), YGNodeLayoutGetHeight(node)},
        LayoutInsets{inset(YGEdgeLeft), inset(YGEdgeTop), inset(YGEdgeRight),
                     inset(YGEdgeBottom)}});

    if (is_grid_container(input.style)) {
        collect_grid_items(node, input, x, y, context, result);
        return;
    }

    std::vector<std::size_t> order;
    const bool permuted = child_layout_order(input, order);
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        // Yoga's child index follows the resolved `order`, so the box walk must use the same
        // permutation to pair each Yoga node with its `LayoutInput`.
        const std::size_t index = permuted ? order[i] : i;
        collect_boxes(YGNodeGetChild(node, i), input.children[index], x, y, context, result);
    }
}

LayoutResult layout_tree(const LayoutInput& root, const LayoutViewport& viewport,
                         float available_width, float available_height,
                         text::TextMeasureCache& cache, bool as_grid_item) {
    std::vector<std::unique_ptr<MeasureContext>> measure_contexts;
    std::vector<std::unique_ptr<GridContext>> grid_contexts;
    YGNodeRef root_node =
        build_tree(root, viewport, cache, measure_contexts, grid_contexts, as_grid_item);
    YGNodeCalculateLayout(root_node, available_width < 0.0f ? YGUndefined : available_width,
                          available_height < 0.0f ? YGUndefined : available_height,
                          root_direction(root));

    LayoutResult result;
    result.boxes.reserve(16);
    const LayoutContext context{&viewport, &cache};
    collect_boxes(root_node, root, 0.0f, 0.0f, context, result);
    YGNodeFreeRecursive(root_node);
    return result;
}

const LayoutInput* find_input(const LayoutInput& root, std::uint64_t id) {
    if (root.id == id) {
        return &root;
    }
    for (const LayoutInput& child : root.children) {
        if (const LayoutInput* found = find_input(child, id)) {
            return found;
        }
    }
    return nullptr;
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
    return layout_tree(root, viewport, viewport.width, viewport.height, *impl_->cache, false);
}

LayoutResult LayoutEngine::layout_subtree(const LayoutInput& root, std::uint64_t id,
                                          const LayoutViewport& viewport) const {
    const LayoutInput* subtree = find_input(root, id);
    if (subtree == nullptr) {
        return {};
    }
    return layout(*subtree, viewport);
}

std::shared_ptr<text::TextMeasureCache> LayoutEngine::text_cache() const {
    return impl_->cache;
}

} // namespace tessera::ui::layout
