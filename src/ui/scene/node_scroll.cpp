// SPDX-License-Identifier: MIT

// Scroll runtime half of `ui::Node` (docs/architecture/god-type-audit-v1.md section 3.1, slice 1:
// move the responsibility blocks into their own TUs with the header unchanged). Everything that
// reads or writes `scroll_bits_`, the content size, the scrollport insets, the offset and its
// smooth-scroll target lives here, so a scroll-feature change no longer edits the TU that also
// holds geometry and interaction policy.

#include "node_internal.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui {

using detail::clamp_inset;

void Node::set_scrollable_x(bool scrollable) {
    const bool current = (scroll_bits_ & kScrollableXBit) != 0;
    if (current == scrollable) {
        return;
    }
    scroll_bits_ = scrollable ? static_cast<std::uint8_t>(scroll_bits_ | kScrollableXBit)
                              : static_cast<std::uint8_t>(scroll_bits_ & ~kScrollableXBit);
    // Toggling the flag changes what the pass and hit testing do with this subtree (a paint clip is
    // emitted or dropped, and points outside the viewport start missing), so it is visible even
    // when the re-clamped offset stays at 0.
    mark_dirty(Dirty::Paint | Dirty::Render);
    set_scroll_offset(scroll_x_, scroll_y_);
}

void Node::set_scrollable_y(bool scrollable) {
    const bool current = (scroll_bits_ & kScrollableYBit) != 0;
    if (current == scrollable) {
        return;
    }
    scroll_bits_ = scrollable ? static_cast<std::uint8_t>(scroll_bits_ | kScrollableYBit)
                              : static_cast<std::uint8_t>(scroll_bits_ & ~kScrollableYBit);
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
    const float max_x = scrollable_x() ? std::max(0.0f, content_width_ - viewport_x) : 0.0f;
    const float max_y = scrollable_y() ? std::max(0.0f, content_height_ - viewport_y) : 0.0f;
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

void Node::set_scroll_behavior(ScrollBehavior behavior) {
    // Policy input only: no paint/layout effect until a programmatic scroll animates an offset, and
    // that move marks Paint|Render through set_scroll_offset.
    scroll_bits_ = behavior == ScrollBehavior::Smooth
                       ? static_cast<std::uint8_t>(scroll_bits_ | kScrollBehaviorSmoothBit)
                       : static_cast<std::uint8_t>(scroll_bits_ & ~kScrollBehaviorSmoothBit);
}

void Node::set_overscroll_behavior(OverscrollBehavior behavior) {
    scroll_bits_ = static_cast<std::uint8_t>(
        (scroll_bits_ & ~kOverscrollBehaviorMask) |
        ((static_cast<std::uint8_t>(behavior) << kOverscrollBehaviorShift) &
         kOverscrollBehaviorMask));
}

void Node::set_overscroll_effect(OverscrollEffect effect) {
    scroll_bits_ = static_cast<std::uint8_t>(
        (scroll_bits_ & ~kOverscrollEffectMask) |
        ((static_cast<std::uint8_t>(effect) << kOverscrollEffectShift) & kOverscrollEffectMask));
}

void Node::set_scroll_animation_target(float x, float y) {
    scroll_animation_target_x_ = std::isfinite(x) ? x : 0.0f;
    scroll_animation_target_y_ = std::isfinite(y) ? y : 0.0f;
    set_flag(kScrollAnimationActive, true);
}

void Node::clear_scroll_animation() {
    set_flag(kScrollAnimationActive, false);
    scroll_animation_target_x_ = scroll_x_;
    scroll_animation_target_y_ = scroll_y_;
}

} // namespace tessera::ui
