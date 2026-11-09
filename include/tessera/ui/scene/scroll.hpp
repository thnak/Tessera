// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

// Scroll container core (docs/specs/scroll-and-gestures-v1.md section 1). Scroll offsets are in
// logical/CSS pixels; the viewport is the node's border box (padding-box clipping is a later
// slice). Content size is supplied explicitly by layout or the caller.
//
// Wheel deltas on `InputEvent` are platform wheel units (Win32 emits one unit per 120-unit notch).
// The default scroll action converts them to logical pixels with this deterministic constant;
// producers that report pixel deltas later can pass unit=1 deltas through their own translation.
inline constexpr float kPixelsPerWheelUnit = 40.0f;

// Maximum offset per axis: `max(0, content - viewport)` when the axis is scrollable, else 0.
[[nodiscard]] float max_scroll_x(const Node& node);
[[nodiscard]] float max_scroll_y(const Node& node);

// Requests an offset. Clamps to [0, max] and returns true only when the offset changed. A
// non-scrollable axis clamps to 0 and never mutates; NaN/infinity are sanitized to 0. Changing the
// offset marks Dirty::Paint | Dirty::Render and never Layout (see Node::set_scroll_offset).
bool scroll_to(Node& node, float x, float y);
bool scroll_by(Node& node, float dx, float dy);

// Nearest scrollable ancestor, including `node` itself; nullptr when the chain has none. This is
// the wheel routing target: the innermost scroll container consumes the wheel and there is no
// nested chaining in this slice (documented in the spec).
[[nodiscard]] Node* scroll_target(Node& node);

// Wheel default action: scroll the nearest scroll container by the wheel deltas converted with
// kPixelsPerWheelUnit. Returns true when an offset changed. The caller must check
// `Event::prevent_default` first; a wheel over a non-scrollable chain changes nothing.
bool apply_wheel_scroll(Node& target, float wheel_x, float wheel_y);

// Minimal scroll-into-view: walks the scrollable ancestors (inner to outer) and adjusts each one
// so the node's world rect intersects the visible viewport. Returns true when any offset changed.
// Not wired into FocusManager yet (focus traversal has no viewport knowledge; see spec
// section 2.1).
bool scroll_into_view(Node& node);

} // namespace tessera::ui
