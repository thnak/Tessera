# Scroll and gestures v1

Status: proposed (Big Step B2, the runtime slice; see [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision:
[ADR-029](../../decisions/ADR-029-interaction-completeness.md) (text editing, scrolling/gestures,
overlays, forms). Companion to
[`input.md`](../design/input.md), [`layout.md`](../design/layout.md),
[`virtualization-v1.md`](virtualization-v1.md), [`mobile-integration.md`](../design/mobile-integration.md),
and [`animation-v1.md`](animation-v1.md) (overscroll/spring-back).

Terms: **V1** is the first release scope ([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8).
**HAL** is the hardware/OS abstraction layer. **Tick** is one fixed-timestep runtime step.
**Damage** is the screen region that must be repainted. **Fling/coast** is scroll momentum after the
pointer is released. **Overscroll** is a visual stretch past a scroll edge. **Gesture arena** is the per-pointer
claim/reject protocol in section 9.1. **Golden** is a deterministic reference image. **UiId** and
**ItemKey** are the numeric node id ([`automation-ids-v1.md`](automation-ids-v1.md)) and the stable
list-item key ([`virtualization-v1.md`](virtualization-v1.md)). **Reduced motion** is the user/OS
preference described in [`accessibility-v1.md`](accessibility-v1.md). Other terms:
[glossary](../reference/glossary.md). Gap ids (`G-06`, `G-07`) refer to rows in
[`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md).

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
    ScrollSnap snap;          // V1: configured via the Scroll API, not CSS scroll-snap-*
};
```

- Content is laid out at its intrinsic size; the scroll container clips to the viewport.
- Scroll offset is a transform, not a relayout; content marks paint/render dirty only.
- `content_size` comes from layout; changes clamp/retain offset deterministically.
- Sticky headers/anchors are resolved in the scroll/layout pass; see §6.
- `Scroll` is the only built-in scrollable container; custom elements manage their own internal
  scroll only if declared (see [`custom-elements-v1.md`](custom-elements-v1.md)).

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
- `scroll_to_key` is the supported way to reveal a list/grid item; see
  [`virtualization-v1.md`](virtualization-v1.md).
- Scroll state is snapshot-testable: offset, velocity, viewport, content size, axis, snap.

## 3. Physics

| Input | Behavior |
|---|---|
| Wheel/trackpad | line/pixel delta, platform acceleration, coalesced |
| Touch/pen drag | 1:1 until release, then fling |
| Keyboard | line/page/home/end; focus-aware |
| Gamepad | not specified for V1 (no decision yet); normalized axis with deadzone |
| Scrollbar | thumb drag, track click, min thumb size |
| Programmatic | animated or immediate; cancellable |

- Fling uses a bounded decay curve; velocity is clamped; no unbounded animation.
- Rubber-band/overscroll is opt-in per axis/container; it never changes content size.
- Paging/snap uses a deterministic target selection; reduced motion makes it immediate.
- Scroll physics are platform-profiled but the state machine is shared.
- No busy-wait; scroll animation is driven by the frame loop and stops at rest.

### 3.1 Overscroll animation (stretch/spring-back)

Normative effect behavior: [`animation-v1.md`](animation-v1.md) §7. Overscroll is a visual state,
not a scroll offset:

```cpp
struct OverscrollState {
    Vec2 visual;        // stretched/translated px, 0 at rest
    Vec2 velocity;
    bool active;
    OverscrollEffect effect;   // None/Glow/Stretch/Bounce/Platform
};
```

- `ScrollState.offset` stays clamped; `visual` is applied as a content-layer transform.
- Drag beyond an edge uses asymptotic resistance:
  `visual = max * raw / (raw + max)`, with `max = min(viewport*ratio, px_cap)`.
- Release runs a damped spring (`-k*visual - c*velocity`) back to zero at fixed dt; stop at
  `|visual| < 0.5 px && |velocity| < 5 px/s`.
- A new pointer down catches the current visual value and cancels the spring without a jump.
- Stretch/bounce applies to content only; sticky headers, scrollbars, and overlays do not
  stretch. No relayout; damage is the old/new visual union.
- Nested scroll: only the last eligible container applies the effect; chaining remains governed
  by `overscroll-behavior`. `contain`/`none` do not animate past the edge.
- Content fitting the viewport, wrong axis, or reduced motion -> no overscroll.
- Platform profile: `Platform` resolves via HAL (iOS/macOS stretch; Android stretch/legacy glow;
  Windows/Linux none for wheel, stretch for touch where supported). Apps may override.
- The effect never changes content size, focus, selection, or automation IDs.

## 4. Nested scroll

- A pointer/wheel event targets the deepest scrollable under the point.
- Scroll chaining: inner consumes until its limit, then outer scrolls if
  `overscroll-behavior` allows (it sets the `allow_chain` field; there is no CSS `scroll-chain` property).
- Capture/target/bubble still applies to input events; `prevent_default` stops default scroll.
- A gesture arena decides between scroll, drag, selection, and custom element capture.
- Nested scroll must not cause layout thrash or double-apply deltas.
- `scroll-behavior: auto|smooth` and `overscroll-behavior: auto|contain|none` are style/runtime
  inputs, not component logic (stylesheet side: [`stylesheet-v1.md`](stylesheet-v1.md) section 11).

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
- Custom elements may consume raw pointer events or use recognizers; see [`custom-elements-v1.md`](custom-elements-v1.md) section 8.
- Gesture handlers run on the UI thread, at tick boundaries; no layout/render/block.
- Reduced motion/disability alternatives: keyboard/accessibility actions for scroll/drag.

### 5.1 Implemented arena slice

`include/tessera/ui/gesture.hpp` + `src/ui/input/gesture_*.cpp` implement the recognition model and
arena as a pure, scene-independent state machine (no scroll integration, no platform translation):

- Model: `GestureKind { Tap, DoubleTap, LongPress, Drag, Swipe, Pinch, Rotate }`,
  `GesturePhase { Began, Changed, Ended, Canceled }`, `GestureEvent`, and `GestureConfig`.
- `GestureArena` consumes normalized `InputEvent`s plus `tick(timestamp_s)` for time-driven
  recognizers. It keeps up to 8 recognizers and 4 per-pointer slots in fixed arrays, emits through
  a bounded 32-event sink, and never allocates, reads the wall clock, or uses RTTI. Per-pointer
  state is keyed by `pointer_id` only; a second pointer joins while `simultaneous` is true
  (otherwise a pointer down during an active press is ignored).
- Protocol: recognizers are polled in registration order and return Ignore/Claim/Reject. The first
  claim wins its pointer; a two-pointer recognizer wins every active pointer. Losers receive
  `on_claim_lost(pointer_id)` to drop per-pointer state without emitting events, and only the
  winner receives subsequent move/up. `PointerCancel` is first-class: a winner that emitted `Began`
  emits `Canceled`; a press canceled before any gesture began emits no gesture event. Window
  `Focus`/`Blur` input cancels every active pointer.
- Built-in recognizers: `TapRecognizer` (tap slop, double-tap window), `LongPressRecognizer`
  (timestamp-driven via tick or observed elapsed time), `DragRecognizer` (threshold, `Began` only
  once), `SwipeRecognizer` (distance plus a deterministic event-time velocity window, direction is
  the sign of the final `dx`/`dy`), and `PinchRotateRecognizer` (two pointer ids, scale/rotation,
  emits Pinch and Rotate event streams).
- Defaults: `tap_slop = 8` logical px, `long_press_ms = 500`, `drag_threshold = 8` logical px,
  `double_tap_ms = 300`, `simultaneous = true`. Gesture `dx`/`dy` are the total displacement from
  the press point.
- Determinism: the same event script produces an identical `GestureEvent` log
  (`tests/unit/gesture_arena_tests.cpp`, `tests/unit/gesture_recognizer_tests.cpp`).

Not implemented in this slice: scene/router integration and capture handoff, scroll claiming or
offset policy, tap-slop in device pixels (DPI scaling), platform velocity/tilt/orientation, wheel
and edge-swipe recognizers, OS/system edge gestures, gesture-driven accessibility alternatives, and
any per-frame wiring (callers own `process`/`tick` scheduling).

## 6. Sticky, anchoring, and focus

- Sticky headers are constrained to their scroll container and resolved after offset changes.
- Scroll anchoring keeps the visible content stable when content above changes; opt-in per
  container, disabled during explicit programmatic scroll.
- Focused/selected item is kept visible on layout/content changes when the item still exists
  after the change.
- Virtualized lists realize the focused item before focus/scroll; see
  [`virtualization-v1.md`](virtualization-v1.md) section 7.
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
  overscroll resistance/release/interrupt/reduced motion, keyboard, scroll-into-view,
  sticky/anchoring, focus/selection, virtualized lists, RTL axes, multi-touch pinch/rotate,
  cancel/capture.
- Snapshot offset/velocity/viewport/content size/overscroll visual; golden stretch/bounce/glow,
  sticky/scrollbar/caret cases.
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
  \-> Overscrolling -> SpringBack -> Idle
  \-> Cancelled (pointer cancel/background/layout reset)
```

- `Dragging`: offset follows pointer 1:1 after slop; at an edge, beyond-edge delta feeds
  `OverscrollState.visual` with resistance; offset remains clamped.
- `Coasting`: velocity decays to zero; a new touch cancels within a frame.
- `Snapping`: choose a deterministic target from position/velocity; reduce motion = immediate.
- `Overscrolling`: pointer is beyond the edge; no fling; visual follows resistance.
- `SpringBack`: damped spring from the release visual/velocity to zero; interruptible.
- Every transition clamps offset to `[min,max]`; content size changes re-clamp and may cancel.

Nested scroll algorithm:

```text
delta = input_delta
for node in target_to_root:
    consumed = node.consume(delta)
    delta -= consumed
    if delta == 0 or !node.allow_chain: break
apply overscroll only to the last eligible node
```

- A child at its limit chains only if `overscroll-behavior` allows; `allow_chain` is the internal
  field derived from it, not a CSS property.
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

Gap-closure decisions:

- **G-06**: recognizers/arena live in `ui/input`; a recognizer claims/rejects per pointer and
  the winner owns capture. HAL reserves system back/home/edge gestures; an app may opt into an
  edge gesture only on platforms that allow it (which platforms is not specified). Custom elements consume only declared
  gestures (`capture_pointer` + claim), and internal scroll must be declared.
- **G-07**: overlay scrollbars by default (8 px visual, 24 px hit target); sticky nodes clamp
  within their scroll container; RTL uses logical axes and non-negative start offsets.

## 10. Deferred/open questions

- Exact per-platform physics profiles and accessibility "reduce motion" tuning (no decision yet).
- Momentum handoff between nested native/custom surfaces.
- Scrollbar auto-hide timing and overlay-vs-gutter defaults per platform.
- Pull-to-refresh/custom overscroll effects (library, not core).
- Scroll anchoring heuristics for virtualized lists.
- Pinch/rotate for content vs custom-element content.
