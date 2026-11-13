// SPDX-License-Identifier: MIT

#include "layout_style.hpp"
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

YGNodeRef build_node(const LayoutInput& input, const LayoutViewport& viewport,
                     text::TextMeasureCache& cache,
                     std::vector<std::unique_ptr<MeasureContext>>& contexts) {
    YGNodeRef node = YGNodeNew();
    apply_style(node, input.style, viewport);
    // `field-sizing: content` makes an editable field a measured leaf even when it is empty.
    const bool sizes_to_content = is_keyword(input.style.find("field-sizing"), "content");
    if ((!input.text.empty() || sizes_to_content) && input.children.empty()) {
        contexts.push_back(std::make_unique<MeasureContext>(
            MeasureContext{&input, text::text_style_from(input.style), &cache}));
        YGNodeSetContext(node, contexts.back().get());
        YGNodeSetMeasureFunc(node, measure_text);
    }
    std::vector<std::size_t> order;
    const bool permuted = child_layout_order(input, order);
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        const std::size_t index = permuted ? order[i] : i;
        YGNodeInsertChild(node, build_node(input.children[index], viewport, cache, contexts), i);
    }
    return node;
}

void collect_boxes(YGNodeRef node, const LayoutInput& input, float parent_x, float parent_y,
                   LayoutResult& result) {
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

    std::vector<std::size_t> order;
    const bool permuted = child_layout_order(input, order);
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        // Yoga's child index follows the resolved `order`, so the box walk must use the same
        // permutation to pair each Yoga node with its `LayoutInput`.
        const std::size_t index = permuted ? order[i] : i;
        collect_boxes(YGNodeGetChild(node, i), input.children[index], x, y, result);
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

namespace {

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
