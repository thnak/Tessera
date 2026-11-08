# Input handler and event pipeline

Status: design for Big Step B2 (runtime slice; see
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). The normalized event
model, bounded SPSC queue, and per-tick adjacent-move coalescing are implemented in
`include/tessera/ui/input.hpp` and `src/ui/input/input_queue.cpp`; pointer input dispatch and
capture are implemented in `src/ui/scene/input_dispatch.cpp`, and focus plus keyboard dispatch are
implemented in `include/tessera/ui/scene/focus.hpp` and `src/ui/scene/focus.cpp`. Platform
translation, pseudo-state dispatch, text/IME, and advanced focus policy remain design-only.
Decision record:
[ADR-017](../../decisions/ADR-017-input-and-fixed-step-runtime.md) (input handler and fixed-step runtime).
Terms: IME = input method editor; HAL = hardware/OS abstraction layer ([`hal.md`](hal.md)); tick = one
fixed-step update of the runtime loop; golden = deterministic reference image; SPSC = single-producer
single-consumer queue; V1 = first release (scope rule in
[AGENTS.md Non-negotiables](../../AGENTS.md#non-negotiables) item 8); see
[`glossary.md`](../reference/glossary.md). Companion to [`game-loop.md`](game-loop.md),
[`scene-graph.md`](scene-graph.md), [`ui-model.md`](ui-model.md),
[`accessibility.md`](accessibility.md), [`mobile-integration.md`](mobile-integration.md),
[`scroll-and-gestures-v1.md`](../specs/scroll-and-gestures-v1.md),
[`text-editing-v1.md`](../specs/text-editing-v1.md), and
[`platform-shell-v1.md`](../specs/platform-shell-v1.md).

## 1. Goals

- Normalize native pointer, mouse, touch, keyboard, and text/IME input into one event stream.
- Deliver events through the scene graph using W3C-style capture/target/bubble flow.
- Maintain focus, hover, active, and pointer-capture state without application boilerplate.
- Update the stylesheet pseudo-state dispatcher (`:hover`, `:active`, `:focus`,
  `:focus-visible`, `:disabled`, `:checked`) before style resolution.
- Be deterministic for tests: recorded input scripts and a simulated clock.
- Keep the hot path allocation-free after warm-up.

## 2. Sources and normalization

| Source | Examples | Normalized into |
|---|---|---|
| Pointer | mouse move/down/up, pen | `PointerEvent` |
| Wheel | scroll, trackpad | `WheelEvent` |
| Touch | down/move/up/cancel, multi-touch | `PointerEvent` with pointer id |
| Keyboard | key down/up, repeat, dead keys | `KeyEvent` |
| Text | committed characters | `TextEvent` |
| IME | composition start/update/end | `CompositionEvent` |
| Window | focus/blur, enter/leave, resize, DPI | lifecycle/focus events |
| Gamepad | not in B2; no release named (see section 14) | `GamepadEvent` |
| Custom | element-emitted | `CustomEvent` |

Platform backends (`hal/input`) translate OS messages:

- Windows: `WM_POINTER*`, `WM_MOUSE*`, `WM_KEY*`, `WM_CHAR`, `WM_IME_*`, DPI change.
- Linux/X11: XInput2, `XKB`, RandR, Present.
- Linux/Wayland: `wl_pointer`, `wl_keyboard`, `wl_touch`, `text-input`, `wp_presentation`.
- Android: not implemented; planned in Big Step F (`MotionEvent`, `KeyEvent`, `InputConnection`; see [`mobile-integration.md`](mobile-integration.md)).
- macOS: deferred ([ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md), macOS via MoltenVK).

No platform-specific type may leak into `ui/`.

## 3. Event model

```cpp
enum class PointerButton { Left, Right, Middle, Back, Forward, Other };
enum class KeyCode { /* logical, layout-independent */ };
enum class Modifier { Shift, Control, Alt, Meta, CapsLock, NumLock };

struct InputEvent {
    EventType type;
    double timestamp_s;      // simulated clock in deterministic mode
    std::uint32_t device_id;
    std::uint32_t pointer_id;
    float x, y;              // logical pixels, top-left origin
    float dx, dy;
    float pressure;
    PointerButton button;
    KeyCode key;
    Modifier modifiers;
    std::uint32_t text_codepoint;
    bool repeat;
    bool is_primary;
};
```

Logical coordinates are in the window's logical pixel space. DPI scaling happens in the
platform layer, once. Golden mode uses a fixed scale.

## 4. Queue and sampling

```text
platform thread(s)
   -> bounded lock-free SPSC queue (ADR-017)
   -> input pump on UI thread
   -> coalesce/compress move events
   -> sample once per simulation tick
   -> hit test + propagation + default actions
   -> pseudo-state update
```

Rules:

- Input is sampled **once per simulation tick**, never per render frame.
- Move events between ticks are coalesced to the latest position plus accumulated delta.
- Down/up/key/text/composition events are never dropped; they are ordered by source timestamp.
- The queue is bounded; overflow sets an overflow flag and reports a diagnostic.
- The UI thread drains the queue before `update`; the renderer never reads live devices.
- `:hover` state is updated after the hit-test pass, before style resolution.

## 5. Hit testing and propagation

1. Convert logical coordinates to scene/world coordinates.
2. `Node::hit_test_deep` in paint order (topmost first), using inverse transforms.
3. Respect `pointer-events: none`, visibility, and clipping.
4. Build the path root -> target.
5. Capture phase (root -> target-1), target, bubble phase (target-1 -> root).
6. A handler may call `stop_propagation`; `prevent_default` is a separate flag for default
   actions (focus, text selection, scrolling).
7. Default actions run after propagation if not prevented.

Pointer capture:

- `set_pointer_capture(node)` on pointer down; all subsequent move/up/cancel for that pointer go
  to the capture target and its ancestors, regardless of hit testing.
- Capture is released on up/cancel or explicit release.
- Capture is per pointer id; multi-touch supports independent captures.
- Losing the window/focus cancels capture and active states.

### 5.1 Custom element input

`CustomElement` nodes are normal hit-test/focus targets with element-local handling:

```cpp
EventResult on_event(const ElementEvent&, CustomEventDispatch&);
```

- `ElementEvent` carries the normalized event, element-local logical coordinates, and optional
  `part_id` from `hit_test_part(local_x, local_y)` (3D picking/sub-targets).
- `EventResult::HandledAndStop` stops propagation; `prevent_default` is separate.
- The element may capture the pointer, request focus, and emit typed custom events
  (`select`, `hover`, `cameraChanged`, ...) through `CustomEventDispatch`.
- Custom events are dispatched to generated JSX bindings; payloads are copied, not owned.
- Keyboard/text/IME require `focusable` and focus; declare `input` categories in
  `ui/elements.json`.
- No global event bus, no raw OS events, no blocking in handlers.
- Headless scripts target `uiId` (+ optional `partId`) through this same path.
- See [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## 6. Focus and keyboard

Implemented focus rules (`ui/scene/focus.hpp` + `FocusManager`):

- `focusable` is a node flag (buttons, inputs, custom nodes); it defaults to `false`.
- Focus only lands on a node that is focusable, visible, attached to the scene root, and has no
  invisible ancestor. `FocusManager::request_focus` rejects anything else without changing the
  current focus; `clear_focus` dispatches `Blur` and clears.
- A focus change dispatches `Blur` to the old target before `Focus` to the new target. Both use
  the same capture/target/bubble path as pointer events, so ancestors observe them.
- Tab traversal (`focus_next` / `focus_previous`) visits focusable nodes in depth-first document
  order (pre-order), wrapping at both ends. It skips invisible subtrees and skips non-focusable
  nodes while still visiting their children. With no current focus, forward traversal starts at
  the first candidate and backward traversal starts at the last.
- `InputRouter` owns a `FocusManager`. `KeyDown`/`KeyUp` go to the focused node through the normal
  propagation path; with no focus they are not dispatched. Key events carry `key`, `modifiers`,
  `text_codepoint`, and `repeat`.
- `PointerDown` on a focusable hit target requests focus; a pointer down anywhere else clears
  focus. Pointer capture remains independent and is unchanged by focus changes.
- `FocusManager` stores raw node pointers and does not observe node destruction: it must be
  destroyed before the scene root, and `clear_focus` must be called before destroying the focused
  subtree.

Deferred policy (design intent, not implemented):

- Explicit `tab_index` ordering (including negative values that skip Tab) and `focus-visible`
  heuristics for keyboard-versus-pointer focus.
- `:focus`/`:focus-visible` pseudo-state updates, modal focus trapping/restore, and window-blur
  retention.
- Keyboard shortcuts/global handlers, text input, and IME composition. Text input will go to the
  focused editable node once those event types are implemented.

## 7. Pointer state and pseudo-classes

State machine per pointer:

```text
Idle -> Hover(target) -> Pressed(target, capture) -> Released(target)
```

- `:hover` is set on the deepest hit target (and ancestors, when a `:hover` rule targets an ancestor chain).
- `:active` is set on the press target while a primary button is down.
- `:focus` is independent of pointer state.
- `:disabled`/`:checked` come from node state, not pointer state.
- State changes mark `StyleDirty`; the style resolver then raises layout/paint/render dirty
  domains only if the matched rules change those groups.
- Hover chains must be recomputed on scene changes, not only on pointer moves.

## 8. Text input and IME

- Editable nodes declare `TextInputKind` (single-line, multiline, numeric, password).
- `TextEvent` carries the committed Unicode codepoint(s).
- `CompositionEvent` carries start/update/end plus the preedit string and caret range.
- The platform candidate window is positioned by the platform backend; the UI reports the
  caret rectangle in logical pixels.
- Password fields never emit plaintext to logs or diagnostics.
- IME is platform-specific; deterministic tests use synthetic text/composition events.

## 9. Determinism and recording

- Every event carries a simulated timestamp in deterministic mode.
- A recorded script is a sequence of `(tick, event)` records; replay is exact.
- Synthetic tests can inject events directly into the UI-thread queue.
- No wall-clock reads, random device ids, or OS-dependent ordering in golden mode.
- Pointer coalescing is deterministic: sort by source sequence, take the last position.

## 10. Threading

- V1: input pump and dispatch run on the UI thread.
- Platform callbacks push into a bounded SPSC queue; the UI thread owns consumption.
- No locks while dispatching handlers; a handler must not block.
- Handlers may request a frame (`request_frame`) or mutate UI state; mutations are processed in
  the same tick.
- Raw input is not dispatched from a render thread.

## 11. Metrics

- `events_queued`, `events_dispatched`, `events_coalesced`, `queue_overflow`
- `hit_tests`, `pointer_captures`, `focus_changes`
- `input_to_style_ms`, `input_to_render_ms` (latency)
- `text_events`, `ime_compositions`
- Debug overlay: pointer id/position/target, focus path, capture target.

## 12. Tests

- Hit testing with transforms, clipping, visibility, `pointer-events: none`.
- Capture/target/bubble order; stop propagation; prevent default.
- Focus set/clear and blur-before-focus order; invalid focus rejection.
- Tab traversal order/wrap, hidden-subtree skipping, keyboard routing with stop propagation.
- Pointer capture across move/up/cancel; multi-touch.
- Focus-visible, tab index, and focus trap restore (deferred with the policy above).
- Hover/active state changes and resulting style invalidation.
- Keyboard repeat, shortcuts, text input, IME composition.
- Deterministic replay: identical event script -> identical state and pixels.
- Queue overflow and window blur/cancel behavior.

## 13. Platform notes

- Windows: per-monitor DPI awareness; pointer messages are preferred over legacy mouse messages;
  IME via `WM_IME_*`; focus via `WM_SETFOCUS`/`WM_KILLFOCUS`.
- X11: XInput2 for raw/pointer events; XKB for keyboard layout; RandR for DPI/refresh.
- Wayland: seat/keyboard/pointer/touch; text-input protocol; frame callbacks are not input.
- Android/macOS: deferred (Android: Big Step F; macOS: ADR-010); keep the normalized model platform-neutral.
- Resize/DPI/window lifecycle is normalized separately; see
  [`window-resize.md`](window-resize.md).

## 14. Open questions

- Gesture recognizers/arena (the system that decides which recognizer wins a pointer sequence) live in `ui/input`; system edge/back gestures are HAL policy.
  Remaining: exact claim/reject API and custom element consumption (see
  [`scroll-and-gestures-v1.md`](../specs/scroll-and-gestures-v1.md)).
- Accessibility actions (invoke, set value) mapped into the same event flow (see [`accessibility.md`](accessibility.md)).
- Gamepad and stylus pressure/tilt.
- IME candidate-window styling and positioning across platforms.
- Event timestamp source and clock correlation with the game loop.
