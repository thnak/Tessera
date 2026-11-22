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
void NodeGeometryApi::invalidate_subtree_paint_bounds() const {
    const Node& self = node();
    // The walk is const (issue #733 item 4): `subtree_paint_bounds_` is `mutable`, so a const node
    // can clear its own and every ancestor's cached box without casting the parent chain.
    for (const Node* cursor = &self; cursor != nullptr; cursor = cursor->parent_) {
        cursor->subtree_paint_bounds_ = rhi::Rect{0.0f, 0.0f, -1.0f, 0.0f};
    }
}

rhi::Rect NodeGeometryApi::subtree_paint_bounds() const {
    const Node& self = node();
    if (self.subtree_paint_bounds_.width >= 0.0f) {
        return self.subtree_paint_bounds_; // cached, including the infinite "unbounded" answer
    }

    const float left = paint_reach(self.paint_outsets_.left, self.glass_blur_sigma_);
    const float top = paint_reach(self.paint_outsets_.top, self.glass_blur_sigma_);
    const float right = paint_reach(self.paint_outsets_.right, self.glass_blur_sigma_);
    const float bottom = paint_reach(self.paint_outsets_.bottom, self.glass_blur_sigma_);
    rhi::Rect box{-left, -top, self.bounds_.width + left + right,
                  self.bounds_.height + top + bottom};

    // Text is the one payload that can paint outside its box (wrapped lines advance past the
    // content box and are not clipped). Layout's fit fact on a plain payload is the only proof it
    // stays inside; anything richer leaves the subtree unbounded.
    if (!own_paint_is_bounded(self)) {
        box = unbounded_paint_box();
    }

    for (const auto& child : self.children_) {
        const rhi::Rect child_box = child->subtree_paint_bounds();
        if (!std::isfinite(child_box.width)) {
            box = unbounded_paint_box();
            break;
        }
        // A child's local space is positioned at its box origin and then transformed around it; the
        // parent's scroll offset shifts the whole content. This is the same composition
        // world_transform() performs, one level down.
        const rhi::Mat3 to_parent =
            rhi::Mat3::translation(child->bounds().x - self.scroll_.scroll_x(),
                                   child->bounds().y - self.scroll_.scroll_y()) *
            child->transform_;
        box = union_rect(box, mapped_aabb(to_parent, child_box));
    }

    self.subtree_paint_bounds_ = box;
    return box;
}

void NodeGeometryApi::set_text_fits_box(bool fits) {
    Node& self = node();
    if (self.flag(Node::kTextFitsBox) == fits) {
        return;
    }
    self.set_flag(Node::kTextFitsBox, fits);
    // A layout fact, not a paint-visible mutation, so it marks no dirty domain - but it decides
    // whether this node's text payload is bounded, which the cached subtree box relies on.
    invalidate_subtree_paint_bounds();
}

void NodeGeometryApi::set_bounds(const rhi::Rect& bounds) {
    Node& self = node();
    if (same_rect(self.bounds_, bounds)) {
        return;
    }
    self.bounds_ = bounds;
    self.mark_dirty(Dirty::Paint | Dirty::Render);
    // Insets are stored relative to the box; a smaller box can make them exceed it, and the new
    // viewport can lower the maximum offset. Re-clamping the offset raises its own damage when it
    // moves.
    self.scroll_.reclamp_insets(self.bounds_.width, self.bounds_.height);
    self.set_scroll_offset(self.scroll_.scroll_x(), self.scroll_.scroll_y());
}

void NodeGeometryApi::set_span_bounds(const rhi::Rect& bounds) const {
    const Node& self = node();
    // Derivable paint data, not a layout box: no dirty marks and no scrollport/offset side effects.
    // `const` (issue #733 item 4) because the paint pass derives it through a const scene; the
    // fields it writes are `mutable` for the same reason `subtree_paint_bounds_` is.
    if (!same_rect(self.bounds_, bounds)) {
        self.bounds_ = bounds;
        // The one geometry write that marks no dirty domain, so the cached subtree box (issue #735)
        // must be cleared here explicitly: `bounds_` is part of it.
        invalidate_subtree_paint_bounds();
    }
}

void NodeGeometryApi::set_transform(const rhi::Mat3& transform) {
    Node& self = node();
    if (same_transform(self.transform_, transform)) {
        return;
    }
    self.transform_ = transform;
    self.set_flag(Node::kHasTransform, !same_transform(transform, rhi::Mat3::identity()));
    self.mark_dirty(Dirty::Paint | Dirty::Render);
}

void NodeGeometryApi::set_z_index(int z_index) {
    Node& self = node();
    if (self.z_index_ == z_index) {
        return;
    }
    self.z_index_ = z_index;
    self.set_flag(Node::kHasZIndex, z_index != 0);
    self.mark_dirty(Dirty::Paint | Dirty::Render);
}

void NodeGeometryApi::set_paint_outsets(const PaintOutsets& outsets) {
    Node& self = node();
    const auto sanitize = [](float value) {
        return std::isfinite(value) && value > 0.0f ? value : 0.0f;
    };
    const PaintOutsets next{sanitize(outsets.left), sanitize(outsets.top), sanitize(outsets.right),
                            sanitize(outsets.bottom), outsets.blurred};
    if (next.left == self.paint_outsets_.left && next.top == self.paint_outsets_.top &&
        next.right == self.paint_outsets_.right && next.bottom == self.paint_outsets_.bottom &&
        next.blurred == self.paint_outsets_.blurred) {
        return;
    }
    self.paint_outsets_ = next;
    self.mark_dirty(Dirty::Paint | Dirty::Render);
}

void NodeGeometryApi::set_glass_blur_sigma(float sigma) {
    Node& self = node();
    const float next = std::isfinite(sigma) && sigma > 0.0f ? sigma : 0.0f;
    if (next == self.glass_blur_sigma_) {
        return;
    }
    self.glass_blur_sigma_ = next;
    // Paint-visible: the emitted sigma changes and damage must cover the new dependency region.
    // Layout and hit testing are unaffected (glass is paint only, glass-v1 section 3.4).
    self.mark_dirty(Dirty::Paint | Dirty::Render);
}

void NodeGeometryApi::set_glass_subtree(bool has_glass) {
    Node& self = node();
    // Derived state, not paint-visible on its own: it can only change when a style change already
    // marked the affected node, so it never raises damage by itself.
    self.set_flag(Node::kGlassSubtree, has_glass);
}

rhi::Mat3 NodeGeometryApi::world_transform() const {
    const Node& self = node();
    // bounds are parent-relative (layout writes them that way), so the box origin is part of the
    // local-to-parent transform; transform_ then acts around that box origin (animation/rotation).
    const rhi::Mat3 local =
        rhi::Mat3::translation(self.bounds_.x, self.bounds_.y) * self.transform_;
    if (self.parent_ != nullptr) {
        // Children of a scroll container live in content space: the parent's scroll shifts them
        // by -scroll without changing this node's own bounds or transform.
        return self.parent_->world_transform() *
               rhi::Mat3::translation(-self.parent_->scroll_.scroll_x(),
                                      -self.parent_->scroll_.scroll_y()) *
               local;
    }
    return local;
}

rhi::Rect NodeGeometryApi::world_bounds() const {
    const Node& self = node();
    const rhi::Mat3 world = world_transform();
    // The transform already carries the box origin, so the local rect starts at (0, 0).
    const float xs[4] = {0.0f, self.bounds_.width, self.bounds_.width, 0.0f};
    const float ys[4] = {0.0f, 0.0f, self.bounds_.height, self.bounds_.height};

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

namespace {

// Slack on the subtree-box prune, in box-local px. The box is already a superset of every
// descendant's hit area (each node adds a pixel of reach and children are mapped by their AABB), so
// this only absorbs the float difference between inverting a chain and inverting it one level at a
// time. It can only keep a subtree that could have been skipped, never skip one that could be hit.
constexpr float kPruneSlack = 0.5f;

// True when the affine part of `matrix` is the identity, so mapping a point through it is a plain
// translation by (m[2], m[5]). The common case: most nodes carry no transform.
bool affine_is_identity(const rhi::Mat3& matrix) {
    return matrix.m[0] == 1.0f && matrix.m[1] == 0.0f && matrix.m[3] == 0.0f && matrix.m[4] == 1.0f;
}

} // namespace

Node* Node::hit_test_deep(float x, float y) {
    if (!rendered()) {
        return nullptr;
    }
    // The one full inversion of the call: the entry node may sit anywhere in the tree, so its point
    // is mapped through the whole parent chain. Everything below it maps one level at a time. A
    // node whose world transform is singular has no hit area and neither do its descendants.
    rhi::Mat3 inverse;
    if (!world_transform().try_inverse(inverse)) {
        return nullptr;
    }
    float local_x = 0;
    float local_y = 0;
    inverse.transform_point(x, y, local_x, local_y);
    return hit_test_local(local_x, local_y);
}

Node* Node::hit_test_local(float local_x, float local_y) {
    // A scroll container clips its children to its scrollport (the border box inset by
    // scrollport_insets, so a caller that set them to the border widths gets the padding box):
    // points outside cannot hit anything in the subtree.
    if (scrollable_x() || scrollable_y()) {
        const float min_x = std::min(scroll_.scrollport_left(), bounds_.width);
        const float min_y = std::min(scroll_.scrollport_top(), bounds_.height);
        const float max_x = std::max(min_x, bounds_.width - scroll_.scrollport_right());
        const float max_y = std::max(min_y, bounds_.height - scroll_.scrollport_bottom());
        if (local_x < min_x || local_y < min_y || local_x > max_x || local_y > max_y) {
            // Children are clipped at the scrollport, but the container itself is still hit within
            // its own box (its border/padding area paints there).
            return flag(kPointerEvents) &&
                           rhi::Rect{0.0f, 0.0f, bounds_.width, bounds_.height}.contains(local_x,
                                                                                         local_y)
                       ? this
                       : nullptr;
        }
    }

    // Children live in content space: the parent's scroll shifts them by -scroll, so a point in
    // this node's box space is `scroll` further along in content space.
    const float content_x = local_x + scroll_.scroll_x();
    const float content_y = local_y + scroll_.scroll_y();

    // Topmost first: highest z-index, and later siblings over earlier ones at equal z-index.
    Node* hit = nullptr;
    for_each_by_z(children_, true, [&](Node& child) {
        hit = child.hit_test_in_parent_space(content_x, content_y);
        return hit != nullptr;
    });
    if (hit != nullptr) {
        return hit;
    }

    // `pointer-events: none` skips only this node as a target; the child search above already ran,
    // so a descendant with `auto` is hit normally.
    return flag(kPointerEvents) &&
                   rhi::Rect{0.0f, 0.0f, bounds_.width, bounds_.height}.contains(local_x, local_y)
               ? this
               : nullptr;
}

Node* Node::hit_test_in_parent_space(float parent_x, float parent_y) {
    if (!rendered()) {
        return nullptr;
    }

    // Parent content space -> this node's box space: undo the box origin, then the transform that
    // acts around it (the same composition world_transform() builds).
    float local_x = 0;
    float local_y = 0;
    if (!flag(kHasTransform)) {
        // The common case, decided from the first line alone: no matrix is read.
        local_x = parent_x - bounds_.x;
        local_y = parent_y - bounds_.y;
    } else if (affine_is_identity(transform_)) {
        local_x = parent_x - bounds_.x - transform_.m[2];
        local_y = parent_y - bounds_.y - transform_.m[5];
    } else {
        rhi::Mat3 inverse;
        if (!(rhi::Mat3::translation(bounds_.x, bounds_.y) * transform_).try_inverse(inverse)) {
            return nullptr; // singular: no area here, and none below
        }
        inverse.transform_point(parent_x, parent_y, local_x, local_y);
    }

    // Prune by the cached subtree paint box. It covers every descendant's hit area and the whole
    // subtree when it cannot prove a bound (an infinite box), so a miss here cannot hide a hit.
    const rhi::Rect box = subtree_paint_bounds();
    if (std::isfinite(box.width) &&
        (local_x < box.x - kPruneSlack || local_y < box.y - kPruneSlack ||
         local_x > box.right() + kPruneSlack || local_y > box.bottom() + kPruneSlack)) {
        return nullptr;
    }
    return hit_test_local(local_x, local_y);
}

} // namespace tessera::ui
