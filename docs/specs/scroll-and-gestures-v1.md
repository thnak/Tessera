# Scroll and gestures v1

Status: proposed (Big Step B2). Decision: ADR-029. Companion to
[`input.md`](../design/input.md), [`layout.md`](../design/layout.md),
[`virtualization-v1.md`](virtualization-v1.md), and [`mobile-integration.md`](../design/mobile-integration.md).

Scrolling and gestures are one system: a drag must decide whether it scrolls, selects, or
activates; nested containers must chain predictably; headless input must be deterministic.

## 1. Scroll container

`Scroll` is a retained node with a scrollable content subtree.

```cpp
struct ScrollState {
    Vec2 offset;              // logical px
    Vec2 velocity;            // px/s, physics
    Extent2D content_size;
    Rect viewport;
    bool clip;
    Axis scroll_axes;         // x/y/both
    ScrollPhysics physics;
    ScrollSnap snap;
};
```

- Content is laid out at its intrinsic size; the scroll container clips to the viewport.
- Scroll offset is a transform, not a relayout; content marks paint/render dirty only.
- `content_size` comes from layout; changes clamp/retain offset deterministically.
- Sticky headers/anchors are resolved in the scroll/layout pass; see §6.
- `Scroll` is the only built-in scrollable container; custom elements manage their own internal
  scroll only if declared.

## 2. Scroll state and APIs

```cpp
scroll_to(offset, animated = true);
scroll_by(delta, animated = true);
scroll_into_view(node/ui_id, alignment);
scroll_to_key(item_key, alignment);
stop_scroll();
set_scroll_offset(offset);   // immediate, no animation
```

- Programmatic scroll uses the same clamping/snap/physics as input.
- `scroll_into_view` respects safe area, sticky headers, and virtualized item realization.
- `scroll_to_key` is the supported way to reveal a list/grid item; see virtualization.
- Scroll state is snapshot-testable: offset, velocity, viewport, content size, axis, snap.

## 3. Physics

| Input | Behavior |
|---|---|
| Wheel/trackpad | line/pixel delta, platform acceleration, coalesced |
| Touch/pen drag | 1:1 until release, then fling |
| Keyboard | line/page/home/end; focus-aware |
| Gamepad | later; normalized axis with deadzone |
| Scrollbar | thumb drag, track click, min thumb size |
| Programmatic | animated or immediate; cancellable |

- Fling uses a bounded decay curve; velocity is clamped; no unbounded animation.
- Rubber-band/overscroll is opt-in per axis/container; it never changes content size.
- Paging/snap uses a deterministic target selection; reduced motion makes it immediate.
- Scroll physics are platform-profiled but the state machine is shared.
- No busy-wait; scroll animation is driven by the frame loop and stops at rest.

## 4. Nested scroll

- A pointer/wheel event targets the deepest scrollable under the point.
- Scroll chaining: inner consumes until its limit, then outer scrolls if
  `overscroll-behavior`/`scroll-chain` allows.
- Capture/target/bubble still applies to input events; `prevent_default` stops default scroll.
- A gesture arena decides between scroll, drag, selection, and custom element capture.
- Nested scroll must not cause layout thrash or double-apply deltas.
- `scroll-behavior: auto|smooth` and `overscroll-behavior: auto|contain|none` are style/runtime
  inputs, not component logic.

## 5. Gesture recognition

Recognizers: tap, double-tap, long-press, drag/pan, swipe, pinch, rotate, wheel, edge-swipe.

```cpp
struct GestureConfig {
    float tap_slop;
    float long_press_ms;
    float drag_threshold;
    bool simultaneous;
};
```

Rules:

- Recognizers live in the input/runtime layer; components opt in with handlers.
- Arena: recognizers claim/reject; a scroll container claims pan after slop; a child drag can
  win with capture.
- Pointer capture routes move/up/cancel to the winner; cancel is first-class.
- Multi-touch: per-pointer ids; pinch/rotate use two pointers; no global pointer state.
- Mouse/pen/touch share the normalized pointer model; pressure/tilt/orientation preserved.
- Custom elements may consume raw pointer events or use recognizers; see custom elements spec.
- Gesture handlers run on the UI thread, at tick boundaries; no layout/render/block.
- Reduced motion/disability alternatives: keyboard/accessibility actions for scroll/drag.

## 6. Sticky, anchoring, and focus

- Sticky headers are constrained to their scroll container and resolved after offset changes.
- Scroll anchoring keeps the visible content stable when content above changes; opt-in per
  container, disabled during explicit programmatic scroll.
- Focused/selected item is kept visible on layout/content changes where possible.
- Virtualized lists realize the focused item before focus/scroll; see virtualization.
- Scrollbars are painted overlays; they do not affect layout unless `scrollbar-gutter` is set.

## 7. Rendering and dirty behavior

- Scrolling changes a transform; it does not relayout content.
- Damage is the viewport union of old/new visible content; full redraw only when required.
- Sticky/scrollbar overlays mark their own regions dirty.
- Virtualized containers recycle nodes; scroll offset drives the realized window.
- No per-frame allocation in the steady-state scroll path.

## 8. Accessibility

- Scroll containers expose role, scroll extent, and scroll actions (line/page/start/end).
- Keyboard focus scrolls into view; screen-reader focus does not fight the caret.
- Dragging/gesture-only actions must have keyboard/accessibility equivalents.
- Announce selection changes at a bounded rate; never announce every scroll pixel.

## 9. Determinism and tests

- Fixed clock, DPI, input script; no inertial randomness.
- Test: wheel/trackpad, drag/fling, nested scroll chaining, snap/paging, rubber band,
  keyboard/gamepad, scroll-into-view, sticky/anchoring, focus/selection, virtualized lists,
  RTL axes, multi-touch pinch/rotate, cancel/capture.
- Snapshot offset/velocity/viewport/content size; golden sticky/scrollbar/caret cases.
- Headless gestures use the same recognizer/scroll path; no wall clock.

## 9.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `ScrollNode` | offset, viewport/content size, axes, physics/snap config | Content layout, item realization |
| `ScrollController` | clamp/animate/scroll-to/into-view, anchoring | Gesture recognition |
| `GestureArena` (input) | per-pointer recognizer claims, capture, cancellation | Scroll offset policy |
| `VirtualList/Grid` | realized range, item measurement/keys | Offset/physics |
| `Platform/HAL` | OS edge/back gesture policy, wheel/touch normalization | App scroll semantics |

Gesture arena protocol:

```text
PointerDown: create arena for pointer; register recognizers (tap/drag/scroll/custom)
PointerMove: each recognizer updates; claim(winner) or reject(); arena cancels others
PointerUp: winner finalizes; losers cancel; capture releases
PointerCancel/window blur: cancel all; no callbacks after teardown
```

Precedence:

1. System/OS gestures (back/home/edge) are declared by HAL policy; app cannot swallow them.
2. A focused text selection drag can claim before scroll after its slop is exceeded.
3. A custom element may claim its declared gesture; otherwise the nearest scrollable claims pan.
4. A long-press menu vs drag is decided by the arena, not by both handlers firing.

Scroll state machine:

```text
Idle -> Dragging -> Coasting -> Snapping -> Idle
  \-> Cancelled (pointer cancel/background/layout reset)
```

- `Dragging`: offset follows pointer 1:1 after slop; overscroll applies only if enabled.
- `Coasting`: velocity decays to zero; a new touch cancels within a frame.
- `Snapping`: choose a deterministic target from position/velocity; reduce motion = immediate.
- Every transition clamps to `[min,max]`; content size changes re-clamp and may cancel.

Nested scroll algorithm:

```text
delta = input_delta
for node in target_to_root:
    consumed = node.consume(delta)
    delta -= consumed
    if delta == 0 or !node.allow_chain: break
apply overscroll only to the last eligible node
```

- A child at its limit chains only if `overscroll-behavior`/`scroll-chain` allows.
- Wheel deltas coalesce by tick; touch deltas are not coalesced across down/up.
- Do not re-enter layout during dispatch; content size updates arrive at the next tick.

Sticky/anchoring:

- Sticky offsets are resolved after scroll offset and before paint; pinned nodes are constrained
  to their scroll container.
- Scroll anchoring records the first visible stable key and its offset delta; apply the delta
  after content changes above it, unless a programmatic scroll is active.
- Virtualized lists anchor by `(UiId, ItemKey)` and realize the anchor before measuring.

Failure/edge cases:

- RTL maps logical start/end to physical axes without changing content order.
- Content shrink clamps offset; content grow does not jump unless anchoring says so.
- Hidden/occluded stops animation; resume re-clamps.
- Custom element internal scroll must be declared and must not fight the parent arena.
- Pointer cancel/background cancels drag/coast and releases capture.

Remaining design gaps: arena/system-gesture API (G-06), scrollbar/sticky/RTL details (G-07).

## 10. Open questions

- Exact platform physics profiles and user accessibility settings.
- Scrollbar overlay vs gutter defaults.
- Nested scroll chaining across custom elements/native surfaces.
- Pinch/rotate for content vs custom elements.
- Scroll anchoring heuristics for virtualized lists.
- Momentum handoff between nested containers.
