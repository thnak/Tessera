// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

// Scroll container core (docs/specs/scroll-and-gestures-v1.md section 1). Scroll offsets are in
// logical/CSS pixels; the scrollport (clamp viewport) is the node's border box inset by
// `scrollport_insets`, and the paint pass clips a scroll container to the same box when the insets
// are set (otherwise it falls back to the style-derived padding box). Content size is supplied
// explicitly by layout or the caller.
//
// Wheel deltas on `InputEvent` are platform wheel units (Win32 emits one unit per 120-unit notch).
// The default scroll action converts them to logical pixels with this deterministic constant;
// producers that report pixel deltas later can pass unit=1 deltas through their own translation.
inline constexpr float kPixelsPerWheelUnit = 40.0f;

// Width the overlay scrollbar occupies (docs/specs/scroll-and-gestures-v1.md section 9.1, G-07:
// 8 px visual, 24 px hit target). `scrollbar-gutter: stable` reserves this width inside the
// scrollport in layout; it is a layout metric, never a paint-time constant.
inline constexpr float kOverlayScrollbarWidth = 8.0f;

// Smooth programmatic scroll (scroll-behavior: smooth) is a fixed-step exponential approach:
// remaining distance decays by exp(-dt / tau) per tick and the last sub-epsilon step snaps, so a
// zero-dt call never moves and the result is deterministic for a given dt sequence. Reduced motion
// clamps every smooth request to a jump (docs/specs/animation-v1.md section 7).
inline constexpr float kSmoothScrollTimeConstant = 0.1f;
inline constexpr float kSmoothScrollEpsilon = 0.5f;

// Scrollport extent per axis: `max(0, bounds - insets)`.
[[nodiscard]] float scrollport_width(const Node& node);
[[nodiscard]] float scrollport_height(const Node& node);

// Maximum offset per axis: `max(0, content - viewport)` when the axis is scrollable, else 0.
[[nodiscard]] float max_scroll_x(const Node& node);
[[nodiscard]] float max_scroll_y(const Node& node);

// Requests an offset. Clamps to [0, max] and returns true only when the offset changed. A
// non-scrollable axis clamps to 0 and never mutates; NaN/infinity are sanitized to 0. Changing the
// offset marks Dirty::Paint | Dirty::Render and never Layout (see Node::set_scroll_offset).
// A call that actually moves the container emits exactly one EventType::Scroll to it (capture ->
// target -> bubble) with the applied delta and the resulting offsets/metrics; a clamped-to-zero,
// non-scrollable, or no-op request emits nothing. `scroll_by` delegates to `scroll_to`, so both
// emit once. The raw `Node::set_scroll_offset` setter (re-clamping, recycling) does not emit: it is
// the state-only path.
bool scroll_to(Node& node, float x, float y);
bool scroll_by(Node& node, float dx, float dy);

// Nearest scrollable ancestor, including `node` itself; nullptr when the chain has none. This is
// the wheel routing target: the innermost scroll container consumes the wheel first and the
// remainder chains outward (apply_wheel_scroll, allow_chain).
[[nodiscard]] Node* scroll_target(Node& node);

// Resolves `id` under `root` and returns its nearest scrollable ancestor (including the node),
// or nullptr when the id is unknown, 0, or the chain has no scroll container. Gesture and wheel
// consumers use this instead of storing a raw pointer, so a removed subtree cannot be dereferenced.
[[nodiscard]] Node* scroll_target_for_id(Node& root, NodeId id);

// Wheel default action: scroll the nearest scroll container by the wheel deltas converted with
// kPixelsPerWheelUnit. Returns true when an offset changed. The caller must check
// `Event::prevent_default` first; a wheel over a non-scrollable chain changes nothing.
//
// Chaining (docs/specs/scroll-and-gestures-v1.md section 4): the delta a container cannot consume
// moves to its nearest scrollable ancestor while the container's `overscroll-behavior` allows it
// (the derived `allow_chain`); `contain` and `none` drop the remainder at that edge. Each applied
// move emits its own Scroll event to the container that moved.
bool apply_wheel_scroll(Node& target, float wheel_x, float wheel_y);

// True when `node` may pass an unconsumed delta to its outer scroll container: derived from
// `overscroll-behavior` (`auto`), not from a separate scroll-chain property.
[[nodiscard]] bool allow_chain(const Node& node);

// True when an overscroll effect may run past `node`'s edge: `overscroll-behavior` must be `auto`
// (contain/none do not animate past the edge) and the effect must not be `none`. `Platform` counts
// as "the HAL profile selects the effect", which is true whenever the profile is not `none`.
[[nodiscard]] bool animates_overscroll(const Node& node);

// Programmatic scroll (scroll-to API, docs/specs/scroll-and-gestures-v1.md section 2) honouring
// `scroll-behavior`: `auto` jumps exactly like scroll_to; `smooth` stores a clamped target and lets
// advance_scroll_animation move the offset over fixed steps (so the request itself emits no Scroll
// event). `reduced_motion` clamps smooth to a jump. Returns true when the request moved the offset
// (auto/reduced motion) or started/retargeted an animation (smooth).
bool scroll_to_programmatic(Node& node, float x, float y, bool reduced_motion = false);
bool scroll_by_programmatic(Node& node, float dx, float dy, bool reduced_motion = false);

// Advances a smooth programmatic scroll by one fixed step. Returns true while the offset is still
// animating after this step. A zero/negative dt is a no-op; `reduced_motion` snaps to the target
// and ends the animation. Offsets are re-clamped every step because content/viewport may have
// changed.
bool advance_scroll_animation(Node& node, double dt, bool reduced_motion = false);

// Advances every animated node in the subtree; returns the number of nodes still animating.
std::size_t advance_scroll_animations(Node& root, double dt, bool reduced_motion = false);

// Minimal scroll-into-view: walks the scrollable ancestors (inner to outer) and adjusts each one so
// the node's world rect is inside that ancestor's scrollport (border box inset by its
// scrollport_insets). Already-visible axes are untouched and an oversized node aligns to the start
// edge, so the call is idempotent. Returns true when any offset changed. Each ancestor that moved
// emits one EventType::Scroll through `scroll_to`; a call that finds the node already visible emits
// nothing. Wired into FocusManager::request_focus (see spec section 2.1).
bool scroll_into_view(Node& node);

} // namespace tessera::ui
