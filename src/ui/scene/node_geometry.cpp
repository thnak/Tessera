// SPDX-License-Identifier: MIT

// Geometry half of `ui::Node` (docs/architecture/god-type-audit-v1.md section 3.1, slice 1): the
// box, its span-bounds override, the local transform, the stacking index, paint outsets, the glass
// paint payload, and everything that derives world space from them (world_transform,
// content_transform, world_bounds, hit_test, hit_test_deep).
//
// `hit_test_deep` stays in this TU rather than becoming a virtual or a free function: it is the
// mouse-move hot path, and the audit records that the call it already pays must not grow.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
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

// The reach a node's own paint adds beyond its border box: the synced box-shadow outsets, three
// glass sigmas (the visible tail of the blur) and one pixel of antialiasing/pixel-snap slack. This
// mirrors paint::detail::reaches_cull exactly, so the cached subtree box is a superset of what the
// paint pass emits for the node.
float paint_reach(float outset, float sigma) {
    const float shadow = std::isfinite(outset) && outset > 0.0f ? outset : 0.0f;
    const float glass = 3.0f * (std::isfinite(sigma) && sigma > 0.0f ? sigma : 0.0f);
    return shadow + glass + 1.0f;
}

rhi::Rect unbounded_paint_box() {
    constexpr float kInf = std::numeric_limits<float>::infinity();
    return rhi::Rect{-kInf, -kInf, kInf, kInf};
}

rhi::Rect union_rect(const rhi::Rect& lhs, const rhi::Rect& rhs) {
    if (!std::isfinite(lhs.width) || !std::isfinite(rhs.width)) {
        return unbounded_paint_box();
    }
    const float min_x = std::min(lhs.x, rhs.x);
    const float min_y = std::min(lhs.y, rhs.y);
    const float max_x = std::max(lhs.right(), rhs.right());
    const float max_y = std::max(lhs.bottom(), rhs.bottom());
    return rhi::Rect{min_x, min_y, max_x - min_x, max_y - min_y};
}

// The AABB of `rect` under `matrix`. A local copy of paint::detail::transformed_aabb: this TU is
// the scene layer and must not include a paint header for it.
rhi::Rect mapped_aabb(const rhi::Mat3& matrix, const rhi::Rect& rect) {
    float xs[4];
    float ys[4];
    matrix.transform_point(rect.x, rect.y, xs[0], ys[0]);
    matrix.transform_point(rect.right(), rect.y, xs[1], ys[1]);
    matrix.transform_point(rect.right(), rect.bottom(), xs[2], ys[2]);
    matrix.transform_point(rect.x, rect.bottom(), xs[3], ys[3]);
    const auto [min_x, max_x] = std::minmax_element(xs, xs + 4);
    const auto [min_y, max_y] = std::minmax_element(ys, ys + 4);
    return rhi::Rect{*min_x, *min_y, *max_x - *min_x, *max_y - *min_y};
}

// True when the payload paint would emit for a text node is exactly the plain text run layout
// measured, so `text_fits_box` actually describes it. Mirrors paint::detail::text_payload_is_plain:
// the scene layer cannot include a paint header, and the two must agree or the cached box would
// claim a bound the paint pass does not honour. Spans paint one run per span with that span's own
// style, a decoration paints a caret past the measured line, `max_lines` truncation is measured
// with the full text, and a font override re-measures with metrics layout never saw.
bool text_payload_is_plain(const PaintData& data) {
    return data.spans.empty() && !data.decoration.has_value() && data.max_lines == 0 &&
           data.text_style.font_size <= 0.0f && data.text_style.font_weight <= 0;
}

// True when this node's own paint is bounded by its box. Text is the one payload that can leave it:
// wrapped lines advance past the content box and are not clipped, so only layout's fit fact on a
// plain payload proves containment.
bool own_paint_is_bounded(const Node& node) {
    const auto& data = node.paint_data();
    if (!data.has_value() || data->kind != PaintKind::Text) {
        return true;
    }
    const bool paints_content =
        !data->text.empty() || !data->spans.empty() ||
        (data->decoration.has_value() && data->decoration->composition_active);
    if (!paints_content) {
        return true;
    }
    return node.text_fits_box() && text_payload_is_plain(*data);
}

} // namespace

// Cached subtree paint box (issue #735, slice C).
//
// The box is the union, in this node's local space (origin at its own box origin, the space
// `transform_` acts in), of its own painted box and every descendant's, so the paint pass can
// reject a whole subtree with one test at its root instead of visiting every node in it.
//
// INVALIDATION. The cache is cleared by `mark_dirty(Dirty::Paint)` on the node and on every
// ancestor it already walks, and every mutation that can move a painted pixel marks Paint:
// `set_bounds`, `set_transform`, `set_paint_outsets`, `set_glass_blur_sigma`, `set_scroll_offset`,
// `set_scrollport_insets`, `set_scrollable_x/y`, `set_paint_data`, `set_visible`,
// `set_style_hidden`, `set_z_index`, and `add_child`/`insert_child`/`remove_child`. That is why the
// mandated points need no separate call: they all reach `mark_dirty`. The two mutations that
// deliberately mark no dirty domain are handled explicitly, because they still change the box:
// `set_span_bounds` (derived paint data, no independent size) and `set_text_fits_box` (a layout
// fact that decides whether a text payload is bounded). Nothing else can change a painted box
// without marking Paint, so no stale box survives a mutation.
//
// SOUNDNESS. A node whose own paint can leave its box - a text payload layout has not proved fits -
// makes the whole subtree unbounded, and an unbounded box is never culled. That is the same
// conservative rule paint::detail::own_emission_bounded applies per node, hoisted to the subtree so
// one unbounded descendant disables the skip for every ancestor. A scroll container's children are
// not clipped here (the clip is a style question this layer cannot answer), which only ever makes
// the box larger, never smaller.
void Node::invalidate_subtree_paint_bounds() const {
    // The walk is const (issue #733 item 4): `subtree_paint_bounds_` is `mutable`, so a const node
    // can clear its own and every ancestor's cached box without casting the parent chain.
    for (const Node* node = this; node != nullptr; node = node->parent_) {
        node->subtree_paint_bounds_ = rhi::Rect{0.0f, 0.0f, -1.0f, 0.0f};
    }
}

rhi::Rect Node::subtree_paint_bounds() const {
    if (subtree_paint_bounds_.width >= 0.0f) {
        return subtree_paint_bounds_; // cached, including the infinite "unbounded" answer
    }

    const float left = paint_reach(paint_outsets_.left, glass_blur_sigma_);
    const float top = paint_reach(paint_outsets_.top, glass_blur_sigma_);
    const float right = paint_reach(paint_outsets_.right, glass_blur_sigma_);
    const float bottom = paint_reach(paint_outsets_.bottom, glass_blur_sigma_);
    rhi::Rect box{-left, -top, bounds_.width + left + right, bounds_.height + top + bottom};

    // Text is the one payload that can paint outside its box (wrapped lines advance past the
    // content box and are not clipped). Layout's fit fact on a plain payload is the only proof it
    // stays inside; anything richer leaves the subtree unbounded.
    if (!own_paint_is_bounded(*this)) {
        box = unbounded_paint_box();
    }

    for (const auto& child : children_) {
        const rhi::Rect child_box = child->subtree_paint_bounds();
        if (!std::isfinite(child_box.width)) {
            box = unbounded_paint_box();
            break;
        }
        // A child's local space is positioned at its box origin and then transformed around it; the
        // parent's scroll offset shifts the whole content. This is the same composition
        // world_transform() performs, one level down.
        const rhi::Mat3 to_parent = rhi::Mat3::translation(child->bounds().x - scroll_.scroll_x(),
                                                           child->bounds().y - scroll_.scroll_y()) *
                                    child->transform_;
        box = union_rect(box, mapped_aabb(to_parent, child_box));
    }

    subtree_paint_bounds_ = box;
    return box;
}

void Node::set_text_fits_box(bool fits) {
    if (flag(kTextFitsBox) == fits) {
        return;
    }
    set_flag(kTextFitsBox, fits);
    // A layout fact, not a paint-visible mutation, so it marks no dirty domain - but it decides
    // whether this node's text payload is bounded, which the cached subtree box relies on.
    invalidate_subtree_paint_bounds();
}

void Node::set_bounds(const rhi::Rect& bounds) {
    if (same_rect(bounds_, bounds)) {
        return;
    }
    bounds_ = bounds;
    mark_dirty(Dirty::Paint | Dirty::Render);
    // Insets are stored relative to the box; a smaller box can make them exceed it, and the new
    // viewport can lower the maximum offset. Re-clamping the offset raises its own damage when it
    // moves.
    scroll_.reclamp_insets(bounds_.width, bounds_.height);
    set_scroll_offset(scroll_.scroll_x(), scroll_.scroll_y());
}

void Node::set_span_bounds(const rhi::Rect& bounds) const {
    // Derivable paint data, not a layout box: no dirty marks and no scrollport/offset side effects.
    // `const` (issue #733 item 4) because the paint pass derives it through a const scene; the
    // fields it writes are `mutable` for the same reason `subtree_paint_bounds_` is.
    if (!same_rect(bounds_, bounds)) {
        bounds_ = bounds;
        // The one geometry write that marks no dirty domain, so the cached subtree box (issue #735)
        // must be cleared here explicitly: `bounds_` is part of it.
        invalidate_subtree_paint_bounds();
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
               rhi::Mat3::translation(-parent_->scroll_.scroll_x(), -parent_->scroll_.scroll_y()) *
               local;
    }
    return local;
}

rhi::Mat3 Node::content_transform() const {
    return world_transform() * rhi::Mat3::translation(-scroll_.scroll_x(), -scroll_.scroll_y());
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
        const float min_x = std::min(scroll_.scrollport_left(), bounds_.width);
        const float min_y = std::min(scroll_.scrollport_top(), bounds_.height);
        const float max_x = std::max(min_x, bounds_.width - scroll_.scrollport_right());
        const float max_y = std::max(min_y, bounds_.height - scroll_.scrollport_bottom());
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
