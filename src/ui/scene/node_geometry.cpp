// SPDX-License-Identifier: MIT

// Geometry half of `ui::Node` (docs/architecture/god-type-audit-v1.md section 3.1, slice 1): the
// box, its span-bounds override, the local transform, the stacking index, paint outsets, the glass
// paint payload, and everything that derives world space from them (world_transform,
// content_transform, world_bounds, hit_test, hit_test_deep).
//
// `hit_test_deep` stays in this TU rather than becoming a virtual or a free function: it is the
// mouse-move hot path, and the audit records that the call it already pays must not grow.

#include "node_internal.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/z_order.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tessera::ui {
namespace {

bool same_rect(const rhi::Rect& lhs, const rhi::Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

bool same_transform(const rhi::Mat3& lhs, const rhi::Mat3& rhs) {
    for (int i = 0; i < 9; ++i) {
        if (lhs.m[i] != rhs.m[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

void Node::set_bounds(const rhi::Rect& bounds) {
    if (same_rect(bounds_, bounds)) {
        return;
    }
    bounds_ = bounds;
    mark_dirty(Dirty::Paint | Dirty::Render);
    // Insets are stored relative to the box; a smaller box can make them exceed it.
    clamp_scrollport_insets();
    // A viewport change can lower the maximum offset; never leave the stored offset out of range.
    set_scroll_offset(scroll_x_, scroll_y_);
}

void Node::set_span_bounds(const rhi::Rect& bounds) {
    // Derivable paint data, not a layout box: no dirty marks and no scrollport/offset side effects.
    if (!same_rect(bounds_, bounds)) {
        bounds_ = bounds;
    }
}

void Node::set_transform(const rhi::Mat3& transform) {
    if (same_transform(transform_, transform)) {
        return;
    }
    transform_ = transform;
    mark_dirty(Dirty::Paint | Dirty::Render);
}

void Node::set_z_index(int z_index) {
    if (z_index_ == z_index) {
        return;
    }
    z_index_ = z_index;
    mark_dirty(Dirty::Paint | Dirty::Render);
}

void Node::set_paint_outsets(const PaintOutsets& outsets) {
    const auto sanitize = [](float value) {
        return std::isfinite(value) && value > 0.0f ? value : 0.0f;
    };
    const PaintOutsets next{sanitize(outsets.left), sanitize(outsets.top), sanitize(outsets.right),
                            sanitize(outsets.bottom), outsets.blurred};
    if (next.left == paint_outsets_.left && next.top == paint_outsets_.top &&
        next.right == paint_outsets_.right && next.bottom == paint_outsets_.bottom &&
        next.blurred == paint_outsets_.blurred) {
        return;
    }
    paint_outsets_ = next;
    mark_dirty(Dirty::Paint | Dirty::Render);
}

void Node::set_glass_blur_sigma(float sigma) {
    const float next = std::isfinite(sigma) && sigma > 0.0f ? sigma : 0.0f;
    if (next == glass_blur_sigma_) {
        return;
    }
    glass_blur_sigma_ = next;
    // Paint-visible: the emitted sigma changes and damage must cover the new dependency region.
    // Layout and hit testing are unaffected (glass is paint only, glass-v1 section 3.4).
    mark_dirty(Dirty::Paint | Dirty::Render);
}

void Node::set_glass_subtree(bool has_glass) {
    // Derived state, not paint-visible on its own: it can only change when a style change already
    // marked the affected node, so it never raises damage by itself.
    set_flag(kGlassSubtree, has_glass);
}

rhi::Mat3 Node::world_transform() const {
    // bounds are parent-relative (layout writes them that way), so the box origin is part of the
    // local-to-parent transform; transform_ then acts around that box origin (animation/rotation).
    const rhi::Mat3 local = rhi::Mat3::translation(bounds_.x, bounds_.y) * transform_;
    if (parent_ != nullptr) {
        // Children of a scroll container live in content space: the parent's scroll shifts them
        // by -scroll without changing this node's own bounds or transform.
        return parent_->world_transform() *
               rhi::Mat3::translation(-parent_->scroll_x_, -parent_->scroll_y_) * local;
    }
    return local;
}

rhi::Mat3 Node::content_transform() const {
    return world_transform() * rhi::Mat3::translation(-scroll_x_, -scroll_y_);
}

rhi::Rect Node::world_bounds() const {
    const rhi::Mat3 world = world_transform();
    // The transform already carries the box origin, so the local rect starts at (0, 0).
    const float xs[4] = {0.0f, bounds_.width, bounds_.width, 0.0f};
    const float ys[4] = {0.0f, 0.0f, bounds_.height, bounds_.height};

    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();

    for (int i = 0; i < 4; ++i) {
        float x = 0;
        float y = 0;
        world.transform_point(xs[i], ys[i], x, y);
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }

    return rhi::Rect{min_x, min_y, max_x - min_x, max_y - min_y};
}

bool Node::hit_test(float x, float y) const {
    if (!rendered()) {
        return false;
    }

    rhi::Mat3 inverse;
    if (!world_transform().try_inverse(inverse)) {
        return false;
    }

    float local_x = 0;
    float local_y = 0;
    inverse.transform_point(x, y, local_x, local_y);
    // Box-local space starts at the origin; bounds_.x/.y only position the box in its parent.
    return rhi::Rect{0.0f, 0.0f, bounds_.width, bounds_.height}.contains(local_x, local_y);
}

Node* Node::hit_test_deep(float x, float y) {
    if (!rendered()) {
        return nullptr;
    }

    // A scroll container clips its children to its scrollport (the border box inset by
    // scrollport_insets, so a caller that set them to the border widths gets the padding box):
    // points outside cannot hit anything in the subtree. The offset itself is applied by
    // world_transform(), so child inverse transforms already account for it.
    if (scrollable_x() || scrollable_y()) {
        rhi::Mat3 inverse;
        if (!world_transform().try_inverse(inverse)) {
            return nullptr;
        }
        float local_x = 0;
        float local_y = 0;
        inverse.transform_point(x, y, local_x, local_y);
        const float min_x = std::min(scrollport_left_, bounds_.width);
        const float min_y = std::min(scrollport_top_, bounds_.height);
        const float max_x = std::max(min_x, bounds_.width - scrollport_right_);
        const float max_y = std::max(min_y, bounds_.height - scrollport_bottom_);
        if (local_x < min_x || local_y < min_y || local_x > max_x || local_y > max_y) {
            // Children are clipped at the scrollport, but the container itself is still hit within
            // its own box (its border/padding area paints there).
            return flag(kPointerEvents) && hit_test(x, y) ? this : nullptr;
        }
    }

    // Topmost first: highest z-index, and later siblings over earlier ones at equal z-index.
    Node* hit = nullptr;
    for_each_by_z(children_, true, [&](Node& child) {
        hit = child.hit_test_deep(x, y);
        return hit != nullptr;
    });
    if (hit != nullptr) {
        return hit;
    }

    // `pointer-events: none` skips only this node as a target; the child search above already ran,
    // so a descendant with `auto` is hit normally.
    return flag(kPointerEvents) && hit_test(x, y) ? this : nullptr;
}

} // namespace tessera::ui
