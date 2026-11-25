// SPDX-License-Identifier: MIT
//
// The box walk over the retained tree. Mirrors `collect_boxes` in layout_cold.cpp box for box, but
// reads the facts the retained tree owns instead of a `LayoutInput`: the id, the text, its resolved
// `text::TextStyle`, the padding the text-fit fact uses, and a grid container's owned subtree
// input. The child order is already the layout order, so no `order` permutation runs here.

#include "layout_measure.hpp"
#include "layout_retained.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <algorithm>
#include <cstddef>

#include <yoga/Yoga.h>

namespace tessera::ui::layout::detail {
namespace {

void collect_grid_items(const RetainedNode& retained, float x, float y,
                        const RetainedTreeState& state, LayoutResult& result) {
    YGNodeRef node = retained.node;
    const auto inset = [node](YGEdge edge) {
        return YGNodeLayoutGetPadding(node, edge) + YGNodeLayoutGetBorder(node, edge);
    };
    const float left = inset(YGEdgeLeft);
    const float top = inset(YGEdgeTop);
    if (retained.grid_input == nullptr) {
        return;
    }
    GridContentBox content;
    content.width = std::max(0.0f, YGNodeLayoutGetWidth(node) - left - inset(YGEdgeRight));
    content.height = std::max(0.0f, YGNodeLayoutGetHeight(node) - top - inset(YGEdgeBottom));
    content.width_definite = true;
    content.height_definite = true;
    float out_width = 0.0f;
    float out_height = 0.0f;
    run_grid(*retained.grid_input, state.viewport, content, *state.cache, x + left, y + top,
             &result.boxes, out_width, out_height);
}

} // namespace

void collect_retained_boxes(const RetainedNode& retained, float parent_x, float parent_y,
                            const RetainedTreeState& state, LayoutResult& result) {
    YGNodeRef node = retained.node;
    if (!retained.box_valid || YGNodeGetHasNewLayout(node)) {
        const auto inset = [node](YGEdge edge) {
            return YGNodeLayoutGetPadding(node, edge) + YGNodeLayoutGetBorder(node, edge);
        };
        retained.box_left = YGNodeLayoutGetLeft(node);
        retained.box_top = YGNodeLayoutGetTop(node);
        retained.box_width = YGNodeLayoutGetWidth(node);
        retained.box_height = YGNodeLayoutGetHeight(node);
        retained.box_insets = LayoutInsets{inset(YGEdgeLeft), inset(YGEdgeTop), inset(YGEdgeRight),
                                           inset(YGEdgeBottom)};
        retained.box_valid = true;
        YGNodeSetHasNewLayout(node, false);
    }
    const float x = parent_x + retained.box_left;
    const float y = parent_y + retained.box_top;
    LayoutBox box{retained.id, rhi::Rect{x, y, retained.box_width, retained.box_height},
                  retained.box_insets};
    const std::uint64_t generation = state.cache->generation();
    if (retained.fit_valid && retained.fit_generation == generation &&
        retained.fit_width == box.bounds.width && retained.fit_height == box.bounds.height) {
        box.text_fit = retained.fit_result;
    } else {
        TextFitInputs inputs;
        inputs.text = retained.text;
        inputs.style = &retained.text_style;
        inputs.text_wrap = retained.text_wrap;
        inputs.display_none = retained.display_none;
        inputs.pad_left = retained.pad_left;
        inputs.pad_top = retained.pad_top;
        inputs.pad_right = retained.pad_right;
        inputs.pad_bottom = retained.pad_bottom;
        box.text_fit = measure_text_fit(inputs, box.bounds, *state.cache);
        retained.fit_valid = true;
        retained.fit_result = box.text_fit;
        retained.fit_width = box.bounds.width;
        retained.fit_height = box.bounds.height;
        retained.fit_generation = generation;
    }
    result.boxes.push_back(box);

    if (retained.grid) {
        collect_grid_items(retained, x, y, state, result);
        return;
    }
    // `children` is already in layout order, so the Yoga child index and the retained child index
    // agree without a permutation.
    for (const RetainedNode* child : retained.children) {
        collect_retained_boxes(*child, x, y, state, result);
    }
}

} // namespace tessera::ui::layout::detail
