// SPDX-License-Identifier: MIT
//
// `NodeScroll` - the geometry half of one scene node's scroll state
// (docs/architecture/god-type-audit-v1.md section 3.1, slice 2): content size, scrollport insets,
// the clamped offset and the smooth-scroll target. The clamp arithmetic lives with the state it
// guards; the gesture policy stays in `src/ui/scene/scroll*.cpp`, and the `Node` forwarders at the
// bottom of this file own the scroll flags and turn every accepted change into Paint|Render marks.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/node_scroll.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui {
namespace {

// One scrollport inset: negative, NaN, and infinite values sanitize to 0 and the inset never takes
// more than the box extent left for it.
float clamp_inset(float value, float extent) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, std::max(0.0f, extent));
}

} // namespace

void NodeScroll::set_content_size(float width, float height) {
    content_width_ = std::max(0.0f, std::isfinite(width) ? width : 0.0f);
    content_height_ = std::max(0.0f, std::isfinite(height) ? height : 0.0f);
}

bool NodeScroll::set_scrollport_insets(float left, float top, float right, float bottom,
                                       float box_width, float box_height) {
    const float next_left = clamp_inset(left, box_width);
    const float next_top = clamp_inset(top, box_height);
    const float next_right = clamp_inset(right, box_width - next_left);
    const float next_bottom = clamp_inset(bottom, box_height - next_top);
    if (next_left == scrollport_left_ && next_top == scrollport_top_ &&
        next_right == scrollport_right_ && next_bottom == scrollport_bottom_) {
        return false;
    }
    scrollport_left_ = next_left;
    scrollport_top_ = next_top;
    scrollport_right_ = next_right;
    scrollport_bottom_ = next_bottom;
    return true;
}

void NodeScroll::reclamp_insets(float box_width, float box_height) {
    const float left = scrollport_left_;
    const float top = scrollport_top_;
    scrollport_left_ = clamp_inset(left, box_width);
    scrollport_top_ = clamp_inset(top, box_height);
    scrollport_right_ = clamp_inset(scrollport_right_, box_width - scrollport_left_);
    scrollport_bottom_ = clamp_inset(scrollport_bottom_, box_height - scrollport_top_);
}

bool NodeScroll::set_scroll_offset(float x, float y, float box_width, float box_height,
                                   bool scrollable_x, bool scrollable_y) {
    const float viewport_x = std::max(0.0f, box_width - scrollport_left_ - scrollport_right_);
    const float viewport_y = std::max(0.0f, box_height - scrollport_top_ - scrollport_bottom_);
    const float max_x = scrollable_x ? std::max(0.0f, content_width_ - viewport_x) : 0.0f;
    const float max_y = scrollable_y ? std::max(0.0f, content_height_ - viewport_y) : 0.0f;
    const float next_x = std::clamp(std::isfinite(x) ? x : 0.0f, 0.0f, max_x);
    const float next_y = std::clamp(std::isfinite(y) ? y : 0.0f, 0.0f, max_y);
    if (next_x == scroll_x_ && next_y == scroll_y_) {
        return false;
    }
    scroll_x_ = next_x;
    scroll_y_ = next_y;
    return true;
}

void NodeScroll::set_animation_target(float x, float y) {
    animation_target_x_ = std::isfinite(x) ? x : 0.0f;
    animation_target_y_ = std::isfinite(y) ? y : 0.0f;
}

void NodeScroll::clear_animation() {
    animation_target_x_ = scroll_x_;
    animation_target_y_ = scroll_y_;
}

// --- Node forwarders ------------------------------------------------------------------------
//
// The public `Node` scroll surface did not move, so no call site changed. The scrollable-axis flags
// and the resolved scroll-behavior/overscroll enums stay in `Node::scroll_bits_`: they are node
// state like every other flag, and putting that byte on `NodeScroll` made the type 44 bytes with 3
// of tail padding, which pushed `sizeof(Node)` from 320 to 328.

void NodeScrollApi::set_scrollable_x(bool scrollable) {
    Node& self = node();
    if (scrollable_x() == scrollable) {
        return;
    }
    self.scroll_bits_ = scrollable
                            ? static_cast<std::uint8_t>(self.scroll_bits_ | Node::kScrollableXBit)
                            : static_cast<std::uint8_t>(self.scroll_bits_ & ~Node::kScrollableXBit);
    // The paint clip and hit-testing clipping start or stop at this node, so it is visible even
    // when the re-clamped offset stays at 0.
    self.mark_dirty(Dirty::Paint | Dirty::Render);
    set_scroll_offset(self.scroll_.scroll_x(), self.scroll_.scroll_y());
}

void NodeScrollApi::set_scrollable_y(bool scrollable) {
    Node& self = node();
    if (scrollable_y() == scrollable) {
        return;
    }
    self.scroll_bits_ = scrollable
                            ? static_cast<std::uint8_t>(self.scroll_bits_ | Node::kScrollableYBit)
                            : static_cast<std::uint8_t>(self.scroll_bits_ & ~Node::kScrollableYBit);
    self.mark_dirty(Dirty::Paint | Dirty::Render);
    set_scroll_offset(self.scroll_.scroll_x(), self.scroll_.scroll_y());
}

void NodeScrollApi::set_content_size(float width, float height) {
    Node& self = node();
    const float next_width = std::max(0.0f, std::isfinite(width) ? width : 0.0f);
    const float next_height = std::max(0.0f, std::isfinite(height) ? height : 0.0f);
    if (self.scroll_.content_width() == next_width &&
        self.scroll_.content_height() == next_height) {
        return;
    }
    self.scroll_.set_content_size(width, height);
    // Clamping only; a content-size change is not itself a layout request.
    set_scroll_offset(self.scroll_.scroll_x(), self.scroll_.scroll_y());
}

void NodeScrollApi::set_scrollport_insets(float left, float top, float right, float bottom) {
    Node& self = node();
    if (!self.scroll_.set_scrollport_insets(left, top, right, bottom, self.bounds_.width,
                                            self.bounds_.height)) {
        return;
    }
    // The paint clip and the hit-test viewport shrink or grow with the insets; the clamp range
    // changes with the viewport, so re-clamp the stored offset (Paint|Render either way).
    self.mark_dirty(Dirty::Paint | Dirty::Render);
    set_scroll_offset(self.scroll_.scroll_x(), self.scroll_.scroll_y());
}

bool NodeScrollApi::set_scroll_offset(float x, float y) {
    Node& self = node();
    if (!self.scroll_.set_scroll_offset(x, y, self.bounds_.width, self.bounds_.height,
                                        scrollable_x(), scrollable_y())) {
        return false;
    }
    // Scrolling moves content, never layout: repaint and re-render only.
    self.mark_dirty(Dirty::Paint | Dirty::Render);
    return true;
}

void NodeScrollApi::set_scroll_behavior(ScrollBehavior behavior) {
    Node& self = node();
    // Policy input only: no paint/layout effect until a programmatic scroll animates an offset, and
    // that move marks Paint|Render through set_scroll_offset.
    self.scroll_bits_ =
        behavior == ScrollBehavior::Smooth
            ? static_cast<std::uint8_t>(self.scroll_bits_ | Node::kScrollBehaviorSmoothBit)
            : static_cast<std::uint8_t>(self.scroll_bits_ & ~Node::kScrollBehaviorSmoothBit);
}

void NodeScrollApi::set_overscroll_behavior(OverscrollBehavior behavior) {
    Node& self = node();
    self.scroll_bits_ = static_cast<std::uint8_t>(
        (self.scroll_bits_ & ~Node::kOverscrollBehaviorMask) |
        ((static_cast<std::uint8_t>(behavior) << Node::kOverscrollBehaviorShift) &
         Node::kOverscrollBehaviorMask));
}

void NodeScrollApi::set_overscroll_effect(OverscrollEffect effect) {
    Node& self = node();
    self.scroll_bits_ = static_cast<std::uint8_t>(
        (self.scroll_bits_ & ~Node::kOverscrollEffectMask) |
        ((static_cast<std::uint8_t>(effect) << Node::kOverscrollEffectShift) &
         Node::kOverscrollEffectMask));
}

void NodeScrollApi::set_scroll_animation_target(float x, float y) {
    Node& self = node();
    self.scroll_.set_animation_target(x, y);
    self.set_flag(Node::kScrollAnimationActive, true);
}

rhi::Mat3 NodeScrollApi::content_transform() const {
    const Node& self = node();
    return self.world_transform() *
           rhi::Mat3::translation(-self.scroll_.scroll_x(), -self.scroll_.scroll_y());
}

void NodeScrollApi::clear_scroll_animation() {
    Node& self = node();
    self.set_flag(Node::kScrollAnimationActive, false);
    self.scroll_.clear_animation();
}

} // namespace tessera::ui
