// SPDX-License-Identifier: MIT
//
// The from-scratch layout path: build a Yoga tree for one `LayoutInput` tree, lay it out, collect
// the boxes, free it. `LayoutEngine::layout` uses the retained tree (layout_retained.cpp); this
// path stays for `layout_subtree`, for grid items, and as the oracle the differential test compares
// the retained tree against.

#include "layout_cold.hpp"
#include "layout_measure.hpp"
#include "layout_style.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <yoga/Yoga.h>

namespace tessera::ui::layout::detail {
namespace {

using detail::apply_block_auto_margin_fill;
using detail::apply_block_child_style;
using detail::apply_root_min_height;
using detail::apply_style;
using detail::is_keyword;
using detail::root_auto_margin_offset;
using detail::with_element_font_size;
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
};

struct LayoutContext {
    const LayoutViewport* viewport = nullptr;
    text::TextMeasureCache* cache = nullptr;
};

YGSize measure_text(YGNodeConstRef node, float width, YGMeasureMode width_mode, float height,
                    YGMeasureMode height_mode) {
    auto* context = static_cast<MeasureContext*>(YGNodeGetContext(node));
    if (context == nullptr || context->cache == nullptr) {
        return YGSize{0.0f, 0.0f};
    }
    return measure_text_box(context->input->text, context->style, *context->cache, width,
                            width_mode, height, height_mode);
}

YGSize measure_grid(YGNodeConstRef node, float width, YGMeasureMode width_mode, float height,
                    YGMeasureMode height_mode) {
    auto* context = static_cast<GridContext*>(YGNodeGetContext(node));
    if (context == nullptr) {
        return YGSize{0.0f, 0.0f};
    }
    return measure_grid_box(*context->input, *context->viewport, *context->cache, width, width_mode,
                            height, height_mode);
}

YGNodeRef build_tree(const LayoutInput& input, const LayoutViewport& viewport,
                     text::TextMeasureCache& cache,
                     std::vector<std::unique_ptr<MeasureContext>>& measure_contexts,
                     std::vector<std::unique_ptr<GridContext>>& grid_contexts, bool as_grid_item,
                     bool block_parent = false) {
    YGNodeRef node = YGNodeNewWithConfig(unrounded_config());
    apply_style(node, input.style, with_element_font_size(viewport, input.style));
    if (block_parent) {
        apply_block_child_style(node); // `margin: 0 auto` fill, no shrink (issue #611)
    }
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
    std::vector<int> keys;
    const bool permuted = child_layout_order(input, order, keys);
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        const std::size_t index = permuted ? order[i] : i;
        YGNodeInsertChild(node,
                          build_tree(input.children[index], viewport, cache, measure_contexts,
                                     grid_contexts, false,
                                     is_keyword(input.style.find("display"), "block")),
                          i);
    }
    return node;
}

YGDirection root_direction(const LayoutInput& root) {
    const std::string* direction = root.style.find("direction");
    return is_keyword(direction, "rtl") ? YGDirectionRTL : YGDirectionLTR;
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
    GridContentBox content;
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
    LayoutBox box{
        input.id, rhi::Rect{x, y, YGNodeLayoutGetWidth(node), YGNodeLayoutGetHeight(node)},
        LayoutInsets{inset(YGEdgeLeft), inset(YGEdgeTop), inset(YGEdgeRight), inset(YGEdgeBottom)}};
    box.text_fit = measure_text_fit(input, box.bounds, *context.cache);
    result.boxes.push_back(box);

    if (is_grid_container(input.style)) {
        collect_grid_items(node, input, x, y, context, result);
        return;
    }

    std::vector<std::size_t> order;
    std::vector<int> keys;
    const bool permuted = child_layout_order(input, order, keys);
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        // Yoga's child index follows the resolved `order`, so the box walk must use the same
        // permutation to pair each Yoga node with its `LayoutInput`.
        const std::size_t index = permuted ? order[i] : i;
        collect_boxes(YGNodeGetChild(node, i), input.children[index], x, y, context, result);
    }
}

} // namespace

LayoutResult layout_cold(const LayoutInput& root, const LayoutViewport& viewport,
                         float available_width, float available_height,
                         text::TextMeasureCache& cache, bool as_grid_item) {
    std::vector<std::unique_ptr<MeasureContext>> measure_contexts;
    std::vector<std::unique_ptr<GridContext>> grid_contexts;
    YGNodeRef root_node =
        build_tree(root, viewport, cache, measure_contexts, grid_contexts, as_grid_item);
    // The root is a page (#611): viewport-wide up to `max-width`, centred, at least viewport tall.
    const bool page = !as_grid_item && available_width >= 0.0f && available_height >= 0.0f;
    if (page) {
        apply_block_auto_margin_fill(root_node, true);
        apply_root_min_height(root_node, available_height);
    }
    YGNodeCalculateLayout(root_node, available_width < 0.0f ? YGUndefined : available_width,
                          page || available_height < 0.0f ? YGUndefined : available_height,
                          root_direction(root));

    LayoutResult result;
    result.boxes.reserve(16);
    const LayoutContext context{&viewport, &cache};
    const float origin_x = page ? root_auto_margin_offset(root_node, available_width) : 0.0f;
    collect_boxes(root_node, root, origin_x, 0.0f, context, result);
    YGNodeFreeRecursive(root_node);
    return result;
}

} // namespace tessera::ui::layout::detail
