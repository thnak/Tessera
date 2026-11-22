// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// The geometry half of one scene node's scroll state: the content size, the scrollport insets, the
// clamped offset and the smooth-scroll target. Extracted from `ui::Node`
// (docs/architecture/god-type-audit-v1.md section 3.1, slice 2) so a scroll-feature change starts
// in its own header.
//
// What is deliberately NOT here:
//   * the scroll *policy* (which gesture moves a container, what a wheel delta becomes, when a
//     smooth scroll ticks) lives in `src/ui/scene/scroll*.cpp`;
//   * the scroll configuration - the scrollable axes and the resolved `scroll-behavior` /
//     `overscroll-*` enums - which is `NodeScrollStyle`, one byte on the node's first cache line.
//
// Keeping those two out is what holds the type to ten floats. The flags byte used to live here,
// which made this type 44 bytes with 3 bytes of tail padding, and that padding alone pushed
// `ui::Node` from 320 to 328 - the exact growth `kNodeSizeBudget` and the `sizeof_node` budget row
// exist to catch. The clamp therefore takes the two scrollable axes as arguments instead of reading
// them from state this type would have to carry.
namespace tessera::ui {

class NodeScroll {
public:
    void set_content_size(float width, float height);
    [[nodiscard]] float content_width() const {
        return content_width_;
    }
    [[nodiscard]] float content_height() const {
        return content_height_;
    }

    [[nodiscard]] float scroll_x() const {
        return scroll_x_;
    }
    [[nodiscard]] float scroll_y() const {
        return scroll_y_;
    }

    // Scrollport insets: how far the visible scrollport is inset from the border box on each side.
    // The clamp viewport is `box - insets`, hit testing clips the subtree to the inset box, and the
    // paint pass clips a scroll container to it when any inset is non-zero (otherwise it falls back
    // to the style-derived padding box). A caller that owns the computed style sets these to the
    // painted border widths so clamping, painting and hit testing agree on a bordered container.
    // All zero by default (the border box); negative/non-finite values sanitize to 0.
    // Returns true when a stored inset changed.
    bool set_scrollport_insets(float left, float top, float right, float bottom, float box_width,
                               float box_height);
    // Re-clamps the stored insets to a box that changed size, without reporting a change: the box
    // setter already raised the paint damage for its own move.
    void reclamp_insets(float box_width, float box_height);
    [[nodiscard]] float scrollport_left() const {
        return scrollport_left_;
    }
    [[nodiscard]] float scrollport_top() const {
        return scrollport_top_;
    }
    [[nodiscard]] float scrollport_right() const {
        return scrollport_right_;
    }
    [[nodiscard]] float scrollport_bottom() const {
        return scrollport_bottom_;
    }
    [[nodiscard]] bool has_scrollport_insets() const {
        return scrollport_left_ > 0.0f || scrollport_top_ > 0.0f || scrollport_right_ > 0.0f ||
               scrollport_bottom_ > 0.0f;
    }

    // Clamps to [0, max(0, content - viewport)] per axis and returns whether the offset changed. An
    // axis that is not scrollable clamps to 0 and never moves, which is why the caller passes the
    // two flags explicitly.
    bool set_scroll_offset(float x, float y, float box_width, float box_height, bool scrollable_x,
                           bool scrollable_y);

    // Smooth programmatic scroll state (scroll-behavior: smooth). `scroll.cpp` owns the policy and
    // the fixed-step advance; the target is a plain offset so retargeting never jumps. Whether a
    // target is active is one bit of `NodeHints` (`scroll_animation_active`), not a separate field
    // here: a bool would be the same padding problem as the flags byte above.
    void set_animation_target(float x, float y);
    // Resets the stored target to the current offset; the owner clears its own active flag.
    void clear_animation();
    [[nodiscard]] float animation_target_x() const {
        return animation_target_x_;
    }
    [[nodiscard]] float animation_target_y() const {
        return animation_target_y_;
    }

private:
    // Ten floats, no trailing byte: see the header comment. `static_assert` in node.hpp pins the
    // sizeof(ui::Node) consequence.
    float content_width_ = 0.0f;
    float content_height_ = 0.0f;
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    float scrollport_left_ = 0.0f;
    float scrollport_top_ = 0.0f;
    float scrollport_right_ = 0.0f;
    float scrollport_bottom_ = 0.0f;
    float animation_target_x_ = 0.0f;
    float animation_target_y_ = 0.0f;
};

} // namespace tessera::ui
