// SPDX-License-Identifier: MIT

#pragma once

// Inline half of `NodeScrollApi`. Included only by `node.hpp`, after `Node` is complete. Do not
// include this file directly.

namespace tessera::ui {

inline Node& NodeScrollApi::node() noexcept {
    return static_cast<Node&>(*this);
}
inline const Node& NodeScrollApi::node() const noexcept {
    return static_cast<const Node&>(*this);
}

inline bool NodeScrollApi::scrollable_x() const {
    return node().scroll_style_.scrollable_x();
}
inline bool NodeScrollApi::scrollable_y() const {
    return node().scroll_style_.scrollable_y();
}
inline float NodeScrollApi::content_width() const {
    return node().scroll_.content_width();
}
inline float NodeScrollApi::content_height() const {
    return node().scroll_.content_height();
}
inline float NodeScrollApi::scroll_x() const {
    return node().scroll_.scroll_x();
}
inline float NodeScrollApi::scroll_y() const {
    return node().scroll_.scroll_y();
}
inline float NodeScrollApi::scrollport_left() const {
    return node().scroll_.scrollport_left();
}
inline float NodeScrollApi::scrollport_top() const {
    return node().scroll_.scrollport_top();
}
inline float NodeScrollApi::scrollport_right() const {
    return node().scroll_.scrollport_right();
}
inline float NodeScrollApi::scrollport_bottom() const {
    return node().scroll_.scrollport_bottom();
}
inline bool NodeScrollApi::has_scrollport_insets() const {
    return node().scroll_.has_scrollport_insets();
}
inline ScrollBehavior NodeScrollApi::scroll_behavior() const {
    return node().scroll_style_.scroll_behavior();
}
inline OverscrollBehavior NodeScrollApi::overscroll_behavior() const {
    return node().scroll_style_.overscroll_behavior();
}
inline OverscrollEffect NodeScrollApi::overscroll_effect() const {
    return node().scroll_style_.overscroll_effect();
}
inline bool NodeScrollApi::scroll_animation_active() const {
    return node().hints_.scroll_animation_active();
}
inline float NodeScrollApi::scroll_animation_target_x() const {
    return node().scroll_.animation_target_x();
}
inline float NodeScrollApi::scroll_animation_target_y() const {
    return node().scroll_.animation_target_y();
}

} // namespace tessera::ui
