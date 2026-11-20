// SPDX-License-Identifier: MIT

#include "layout_style.hpp"

#include <algorithm>

namespace tessera::ui::layout::detail {

namespace {

bool is_auto(YGValue value) {
    return value.unit == YGUnitAuto;
}

// A margin that is not `auto` and not a non-zero length: the common `0 auto` shape.
bool is_zero_or_unset(YGValue value) {
    return value.unit == YGUnitUndefined || (value.unit == YGUnitPoint && value.value == 0.0f);
}

// Points of padding plus border on the left and right, or false when either is a percentage
// (which cannot be folded into a point constraint).
bool horizontal_insets(YGNodeRef node, float& insets) {
    insets = 0.0f;
    for (const YGEdge edge : {YGEdgeLeft, YGEdgeRight}) {
        const YGValue padding = YGNodeStyleGetPadding(node, edge);
        if (padding.unit == YGUnitPercent) {
            return false;
        }
        if (padding.unit == YGUnitPoint) {
            insets += padding.value;
        }
        const float border = YGNodeStyleGetBorder(node, edge);
        if (!YGFloatIsUndefined(border)) {
            insets += border;
        }
    }
    return true;
}

void widen_limit(YGNodeRef node, YGValue limit, float insets,
                 void (*set_points)(YGNodeRef, float)) {
    if (limit.unit == YGUnitPoint) {
        set_points(node, limit.value + insets);
    }
}

} // namespace

void apply_block_auto_margin_fill(YGNodeRef node, bool is_root) {
    if (YGNodeStyleGetPositionType(node) == YGPositionTypeAbsolute ||
        YGNodeStyleGetDisplay(node) == YGDisplayNone || !is_auto(YGNodeStyleGetWidth(node))) {
        return;
    }
    const YGValue left = YGNodeStyleGetMargin(node, YGEdgeLeft);
    const YGValue right = YGNodeStyleGetMargin(node, YGEdgeRight);
    // The root has no container to stretch it: Yoga sizes a root that has a `max-width` to its
    // content, so there the cap alone also asks for the fill.
    const bool any_auto = is_auto(left) || is_auto(right) ||
                          (is_root && YGNodeStyleGetMaxWidth(node).unit == YGUnitPoint);
    const bool others_zero =
        (is_auto(left) || is_zero_or_unset(left)) && (is_auto(right) || is_zero_or_unset(right));
    float insets = 0.0f;
    if (!any_auto || !others_zero || !horizontal_insets(node, insets)) {
        return;
    }

    // A block box with `width: auto` fills its container, then `max-width` caps it and the
    // leftover goes to the auto margins. Yoga instead shrinks an item with an auto margin to its
    // content, so the fill is made explicit: 100% of the container, which Yoga then clamps to
    // max-width and centres. The padding is inside the percentage, so the box is border-box and
    // the limits grow by the insets they were authored without.
    if (YGNodeStyleGetBoxSizing(node) == YGBoxSizingContentBox) {
        widen_limit(node, YGNodeStyleGetMaxWidth(node), insets, YGNodeStyleSetMaxWidth);
        widen_limit(node, YGNodeStyleGetMinWidth(node), insets, YGNodeStyleSetMinWidth);
        YGNodeStyleSetBoxSizing(node, YGBoxSizingBorderBox);
    }
    YGNodeStyleSetWidthPercent(node, 100.0f);
}

float root_auto_margin_offset(YGNodeRef root, float available_width) {
    const bool auto_left = is_auto(YGNodeStyleGetMargin(root, YGEdgeLeft));
    const bool auto_right = is_auto(YGNodeStyleGetMargin(root, YGEdgeRight));
    const float free = std::max(0.0f, available_width - YGNodeLayoutGetWidth(root));
    return auto_left ? (auto_right ? free / 2.0f : free) : 0.0f;
}

void apply_root_min_height(YGNodeRef root, float available_height) {
    if (YGNodeStyleGetHeight(root).unit != YGUnitAuto ||
        YGNodeStyleGetMinHeight(root).unit != YGUnitUndefined) {
        return;
    }
    // A page is at least as tall as the window and grows with its content, as a document does:
    // the root's height is auto with the viewport as its minimum. The minimum is a content-box
    // value unless the root is border-box, so the vertical insets and margins come off it.
    float taken = 0.0f;
    for (const YGEdge edge : {YGEdgeTop, YGEdgeBottom}) {
        const YGValue margin = YGNodeStyleGetMargin(root, edge);
        if (margin.unit == YGUnitPoint) {
            taken += margin.value;
        }
        if (YGNodeStyleGetBoxSizing(root) == YGBoxSizingContentBox) {
            const YGValue padding = YGNodeStyleGetPadding(root, edge);
            if (padding.unit == YGUnitPoint) {
                taken += padding.value;
            }
            const float border = YGNodeStyleGetBorder(root, edge);
            if (!YGFloatIsUndefined(border)) {
                taken += border;
            }
        }
    }
    YGNodeStyleSetMinHeight(root, std::max(0.0f, available_height - taken));
}

} // namespace tessera::ui::layout::detail
