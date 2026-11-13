// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/z_order.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

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

// One scrollport inset: negative, NaN, and infinite values sanitize to 0 and the inset never takes
// more than the box extent left for it.
float clamp_inset(float value, float extent) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, std::max(0.0f, extent));
}

} // namespace

Node::Node(NodeId id)
    : id_(id) {}

Node& Node::add_child(std::unique_ptr<Node> child) {
    child->mark_dirty(Dirty::Style | Dirty::Layout | Dirty::Paint | Dirty::Render);
    child->parent_ = this;
    children_.push_back(std::move(child));
    mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
    refresh_subtree_dirty();
    return *children_.back();
}

Node& Node::insert_child(std::size_t index, std::unique_ptr<Node> child) {
    child->mark_dirty(Dirty::Style | Dirty::Layout | Dirty::Paint | Dirty::Render);
    child->parent_ = this;
    const std::size_t at = std::min(index, children_.size());
    const auto inserted =
        children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(at), std::move(child));
    mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
    refresh_subtree_dirty();
    return **inserted;
}

std::unique_ptr<Node> Node::remove_child(const Node& child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() != &child) {
            continue;
        }
        std::unique_ptr<Node> removed = std::move(*it);
        children_.erase(it);
        removed->parent_ = nullptr;
        mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
        refresh_subtree_dirty();
        return removed;
    }
    return nullptr;
}

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
    glass_subtree_ = has_glass;
}

void Node::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
}

void Node::set_focusable(bool focusable) {
    if (focusable_ == focusable) {
        return;
    }
    focusable_ = focusable;
    mark_dirty(Dirty::Style);
}

void Node::set_tab_index(int tab_index) {
    tab_index_ = tab_index;
}

void Node::set_scrollable_x(bool scrollable) {
    if (scrollable_x_ == scrollable) {
        return;
    }
    scrollable_x_ = scrollable;
    // Toggling the flag changes what the pass and hit testing do with this subtree (a paint clip is
    // emitted or dropped, and points outside the viewport start missing), so it is visible even
    // when the re-clamped offset stays at 0.
    mark_dirty(Dirty::Paint | Dirty::Render);
    set_scroll_offset(scroll_x_, scroll_y_);
}

void Node::set_scrollable_y(bool scrollable) {
    if (scrollable_y_ == scrollable) {
        return;
    }
    scrollable_y_ = scrollable;
    mark_dirty(Dirty::Paint | Dirty::Render);
    set_scroll_offset(scroll_x_, scroll_y_);
}

void Node::set_content_size(float width, float height) {
    const float next_width = std::max(0.0f, width);
    const float next_height = std::max(0.0f, height);
    if (content_width_ == next_width && content_height_ == next_height) {
        return;
    }
    content_width_ = next_width;
    content_height_ = next_height;
    // Clamping only; a content-size change is not itself a layout request.
    set_scroll_offset(scroll_x_, scroll_y_);
}

void Node::clamp_scrollport_insets() {
    scrollport_left_ = clamp_inset(scrollport_left_, bounds_.width);
    scrollport_top_ = clamp_inset(scrollport_top_, bounds_.height);
    scrollport_right_ = clamp_inset(scrollport_right_, bounds_.width - scrollport_left_);
    scrollport_bottom_ = clamp_inset(scrollport_bottom_, bounds_.height - scrollport_top_);
}

void Node::set_scrollport_insets(float left, float top, float right, float bottom) {
    const float next_left = clamp_inset(left, bounds_.width);
    const float next_top = clamp_inset(top, bounds_.height);
    const float next_right = clamp_inset(right, bounds_.width - next_left);
    const float next_bottom = clamp_inset(bottom, bounds_.height - next_top);
    if (next_left == scrollport_left_ && next_top == scrollport_top_ &&
        next_right == scrollport_right_ && next_bottom == scrollport_bottom_) {
        return;
    }
    scrollport_left_ = next_left;
    scrollport_top_ = next_top;
    scrollport_right_ = next_right;
    scrollport_bottom_ = next_bottom;
    // The paint clip and the hit-test viewport shrink or grow with the insets; the clamp range
    // changes with the viewport, so re-clamp the stored offset (Paint|Render either way).
    mark_dirty(Dirty::Paint | Dirty::Render);
    set_scroll_offset(scroll_x_, scroll_y_);
}

bool Node::set_scroll_offset(float x, float y) {
    const float viewport_x = std::max(0.0f, bounds_.width - scrollport_left_ - scrollport_right_);
    const float viewport_y = std::max(0.0f, bounds_.height - scrollport_top_ - scrollport_bottom_);
    const float max_x = scrollable_x_ ? std::max(0.0f, content_width_ - viewport_x) : 0.0f;
    const float max_y = scrollable_y_ ? std::max(0.0f, content_height_ - viewport_y) : 0.0f;
    const float next_x = std::clamp(std::isfinite(x) ? x : 0.0f, 0.0f, max_x);
    const float next_y = std::clamp(std::isfinite(y) ? y : 0.0f, 0.0f, max_y);
    if (next_x == scroll_x_ && next_y == scroll_y_) {
        return false;
    }
    scroll_x_ = next_x;
    scroll_y_ = next_y;
    // Scrolling moves content, never layout: repaint and re-render only.
    mark_dirty(Dirty::Paint | Dirty::Render);
    return true;
}

void Node::set_pseudo_state(style::PseudoState state) {
    if (pseudo_state_ == state) {
        return;
    }
    pseudo_state_ = state;
    mark_dirty(Dirty::Style);
}

void Node::set_pseudo_state(style::PseudoState state, bool enabled) {
    const style::PseudoState next = enabled ? (pseudo_state_ | state) : (pseudo_state_ & ~state);
    set_pseudo_state(next);
}

void Node::mark_dirty(Dirty flags) {
    if (flags == Dirty::Clean) {
        return;
    }

    dirty_ |= flags;
    subtree_dirty_ |= flags;
    for (Node* ancestor = parent_; ancestor != nullptr; ancestor = ancestor->parent_) {
        if ((ancestor->subtree_dirty_ & flags) == flags) {
            break;
        }
        ancestor->subtree_dirty_ |= flags;
    }
}

void Node::clear_dirty(Dirty flags) {
    const Dirty remaining = dirty_ & ~flags;
    if (remaining == dirty_) {
        return;
    }
    dirty_ = remaining;
    refresh_subtree_dirty();
}

void Node::refresh_subtree_dirty() {
    Dirty next = dirty_;
    for (const auto& child : children_) {
        next |= child->subtree_dirty();
    }
    if (next == subtree_dirty_) {
        return;
    }
    subtree_dirty_ = next;
    if (parent_ != nullptr) {
        parent_->refresh_subtree_dirty();
    }
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
    if (!visible_) {
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
    if (!visible_) {
        return nullptr;
    }

    // A scroll container clips its children to its scrollport (the border box inset by
    // scrollport_insets, so a caller that set them to the border widths gets the padding box):
    // points outside cannot hit anything in the subtree. The offset itself is applied by
    // world_transform(), so child inverse transforms already account for it.
    if (scrollable_x_ || scrollable_y_) {
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
            return hit_test(x, y) ? this : nullptr;
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

    return hit_test(x, y) ? this : nullptr;
}

void Node::on_event(Event&) {}

} // namespace tessera::ui
