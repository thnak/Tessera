# Input handler and event pipeline

Status: design for Big Step B2 (runtime slice; see
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). The normalized event
model, bounded SPSC queue, and per-tick adjacent-move coalescing are implemented in
`include/tessera/ui/input.hpp` and `src/ui/input/input_queue.cpp`; pointer input dispatch and
capture are implemented in `src/ui/scene/input_dispatch.cpp`; focus plus keyboard dispatch are
implemented in `include/tessera/ui/scene/focus.hpp` and `src/ui/scene/focus.cpp`; and the
`:focus` pseudo-state bridge to selector matching/computed style is implemented in
`include/tessera/ui/style/scene_style.hpp` and `src/ui/style/scene_style.cpp`. Tab-index ordering,
`:focus` style re-evaluation, and modal focus trapping/restoration are part of that implementation.
Wheel and text routing, window focus/blur handling, the Tab/Shift+Tab default focus action with
`prevent_default`, and the scene-event pointer payload are implemented in
`src/ui/scene/input_dispatch.cpp` and `include/tessera/ui/scene/event.hpp`. The per-tick input pump
(queue -> sampler -> router -> `RuntimeLoop::request_frame`) is
`include/tessera/ui/input_pump.hpp` and `src/ui/input/input_pump.cpp`. Windows platform translation
(pointer, wheel, key and character messages into `InputEvent`, see section 2.1) is implemented in
`src/platform/windows/win32_input.cpp`. Linux/X11 and other platform translation, hover/active
pseudo-state dispatch, `:focus-visible` heuristics, window-blur retention policy (blur already
cancels capture), per-pointer-id capture for multi-touch, IME composition, gestures, and
deterministic recording remain design-only.
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

### 2.1 Windows translation (implemented)

`hal::Window::set_input_queue(ui::InputQueue*)` routes translated input into the application's
queue (not owned; reset it with `nullptr` before destroying it). `Win32InputTranslator` runs on the
window's thread, so the queue's single producer is the UI thread.

- **Pointer:** `WM_MOUSEMOVE`, the left/right/middle/X1/X2 buttons. Coordinates are logical pixels
  (physical client position divided by the window's DPI scale), `dx`/`dy` are the change from the
  previous pointer event, pressure is 0.5 while any button is down. A press captures the mouse
  (`SetCapture`) so the move and release arrive outside the window; losing the capture, focus
  or modal mode emits `PointerCancel` and clears the pressed buttons. A release without a seen
  press is ignored. Shift and Control come from the message, Alt and Meta from the keyboard state.
- **Wheel:** vertical and horizontal, in notches (120 units) with the web sign: positive `dy`
  scrolls content down, positive `dx` scrolls right. The message's screen coordinates are
  converted to client logical pixels.
- **Keys:** `KeyDown`/`KeyUp` with a logical code from `include/tessera/ui/key_code.hpp`
  (upper-case ASCII for letters and digits, `0x100`+ for non-printable keys). Auto-repeat sets
  `repeat` on key down. System keys (Alt combinations, F10) are recorded and still reach the
  default window procedure.
- **Text:** `WM_CHAR` becomes `Text` with the code point; surrogate pairs are combined, an orphan
  half is dropped, and control characters (including Backspace, Enter and Tab) are left to key
  events.
- **Focus:** `WM_SETFOCUS`/`WM_KILLFOCUS` also emit `Focus`/`Blur` input events.
- **Waking:** `wait_for_events` returns `true` while the queue holds unconsumed input, because
  input never becomes a `WindowEvent`; this is what lets a render-on-demand loop sleep until a
  pointer or key event.
- **Not yet:** touch and pen (`WM_POINTER*`), IME composition, dead keys, per-key `code`
  (physical position), pointer leave/enter, and `:hover`/`:active` dispatch.

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

The scene-level `ui::Event` (`include/tessera/ui/scene/event.hpp`) adds `Wheel` and `Text` to the
event types and carries the payload of the normalized event: `button`, `pointer_id`, `pressure`,
`scroll_x`/`scroll_y` (web sign), `timestamp_s`, `is_primary`, and `prevent_default`. `handled`
stays advisory; `prevent_default` is read by the dispatcher after propagation and currently
suppresses only the Tab focus default action. Pointer and wheel fields are zero for key, text, and
focus events.

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

### 4.1 Per-tick pump (implemented)

`ui::InputPump` (`include/tessera/ui/input_pump.hpp`) owns the queue, the `InputSampler`, and the
`InputRouter`. `pump(root, loop)` consumes at most `max_events_per_tick` events (default 256),
coalesces adjacent moves as above, dispatches through the router, and calls
`RuntimeLoop::request_frame()` when at least one event was consumed, so a render-on-demand loop
wakes even when hit testing drops the event. Queue capacity and the budget are constructor
configuration. The per-call counters (`consumed`, `dispatched`, `dropped`, `overflowed`) are
returned and accumulated in `total()`. Events beyond the budget stay queued for the next tick, so
moves split across ticks do not coalesce. The pump never blocks and allocates nothing after
construction.

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

Implemented subset: `InputRouter` owns one capture target, set by the last `PointerDown` and
cleared by `PointerUp`, `PointerCancel`, or an `InputEventType::Focus`/`Blur` input event. There is
no public `set_pointer_capture`, no per-pointer-id capture, and no multi-touch yet. Wheel events are
hit-tested at the cursor and are not delivered to the capture target; text events go to the focused
node; `pointer-events: none`, clipping, and `prevent_default` for scrolling/selection are not
implemented. `prevent_default` is honored for the Tab default action only (section 6).

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
- Focus updates the target's `PseudoState::Focus` bit: the old target loses it before `Blur` and
  the new target gains it before `Focus`. The change marks the `Style` dirty domain so
  `:focus`/`:not(:focus)` rules re-resolve; `ui/style/scene_style.hpp` exposes that state to
  selector matching.
- Sequential traversal order (`focus_next` / `focus_previous`):
  1. focusable nodes with a positive `tab_index`, ascending; equal values keep depth-first
     document order (pre-order);
  2. focusable nodes with `tab_index == 0`, in depth-first document order.
  Both ends wrap. Negative `tab_index` values are skipped by sequential traversal but remain valid
  targets for `request_focus` (programmatic focus). Invisible subtrees are skipped and
  non-focusable nodes are skipped while their children are still visited. With no current focus,
  forward traversal starts at the first candidate and backward traversal starts at the last.
- Modal focus trapping: `FocusManager::set_modal_root(node)` restricts `request_focus`, sequential
  traversal, and pointer-driven focus to `node` and its descendants (the modal root itself is
  included). Installing a trap saves the current focus when it is outside the new subtree and moves
  focus to the first candidate inside it, or clears focus when the subtree has no candidates.
  Focus requests outside the trap return `false` without changing focus. `clear_modal_root()` (or
  `set_modal_root(nullptr)`) releases the trap and restores the saved focus when that node is still
  attached, visible, focusable, and allowed; otherwise the current focus stays where it is.
  `FocusManager::reset()` (used by `InputRouter::reset()`) drops focus and the trap without
  restoring the saved target.
- `FocusManager` resolves nodes by `NodeId` under the scene root on every operation. Removing or
  destroying the modal root without an explicit clear therefore releases the trap on the next
  focus operation and restores the saved focus. It still stores raw pointers, so callers must
  destroy the manager before the scene root and should clear focus before destroying a subtree that
  currently holds focus.
- `InputRouter` owns a `FocusManager`. `KeyDown`/`KeyUp` go to the focused node through the normal
  propagation path; with no focus they are not dispatched. Key events carry `key`, `modifiers`,
  `text_codepoint`, and `repeat`.
- `PointerDown` on a focusable hit target requests focus; a pointer down on a non-focusable target
  inside the trap (or anywhere without an active trap) clears focus. Pointer down outside an
  active trap neither steals nor clears focus. Pointer capture remains independent and is unchanged
  by focus changes.
- Default action (implemented): after a `KeyDown` with `ui::key::kTab` propagates, the router calls
  `focus_next(root)`, or `focus_previous(root)` when Shift is held, unless a handler set
  `Event::prevent_default`. The modal trap is enforced inside `FocusManager`.
  `InputDispatchResult` reports `default_prevented` and `focus_moved`. `KeyUp` for Tab has no
  default action. `Text` input events are delivered to the focused node as `EventType::Text`.
- Window blur (`InputEventType::Blur`) cancels pointer capture; it does not clear scene focus,
  because `FocusManager` remains the owner of the scene-level Focus/Blur events.

Deferred policy (design intent, not implemented):

- `focus-visible` heuristics for keyboard-versus-pointer focus, and window-blur focus retention.
- Keyboard shortcuts/global handlers, editable-node text editing/selection, and IME composition.
  Committed characters already reach the focused node as `Text` events, but there is no editable
  node model behind them yet.

## 7. Pointer state and pseudo-classes

State machine per pointer:

```text
Idle -> Hover(target) -> Pressed(target, capture) -> Released(target)
```

- `:hover` is set on the deepest hit target (and ancestors, when a `:hover` rule targets an ancestor chain).
- `:active` is set on the press target while a primary button is down.
- `:focus` is independent of pointer state; `FocusManager` owns the `PseudoState::Focus` bit and
  clears/sets it around `Blur`/`Focus` dispatch.
- `:disabled`/`:checked` come from node state, not pointer state.
- State changes mark `StyleDirty`; the style resolver then raises layout/paint/render dirty
  domains only if the matched rules change those groups. `SceneStyleTree::compute` refreshes the
  cached pseudo-state from the node on each evaluation, so a focus/blur is reflected by the next
  style resolution without rebuilding the tree.
- Hover chains must be recomputed on scene changes, not only on pointer moves.

Not implemented: the input layer does not set `PseudoState::Hover` or `PseudoState::Active` yet, so
`:hover`/`:active` rules never match on interaction. Only `Focus` is maintained, by `FocusManager`.

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
- Tab traversal order/wrap, positive/zero/negative `tab_index` ordering, hidden-subtree skipping,
  keyboard routing with stop propagation (`tests/unit/focus_tests.cpp`,
  `tests/unit/focus_policy_tests.cpp`).
- Pointer capture across move/up/cancel; multi-touch.
- Modal trapping: focus confined to the subtree for Tab and pointer input, rejected outside
  requests, saved-focus restoration, invalid-saved-focus behavior, and trap release when the modal
  root is removed (`tests/unit/focus_policy_tests.cpp`).
- `:focus`/`:not(:focus)` selector matching and style re-evaluation, including `Style` dirty
  marking (`tests/unit/focus_style_tests.cpp`).
- Hover/active state changes and resulting style invalidation (not implemented yet).
- Keyboard repeat, shortcuts, IME composition (only committed `Text` routing exists).
- Deterministic replay: identical event script -> identical state and pixels (not implemented).
- Queue overflow and window blur/cancel behavior.
- Scene payload mapping: button, pointer id, pressure, wheel deltas, timestamp
  (`tests/unit/input_pipeline_tests.cpp`).
- Wheel hit-testing/bubbling, text delivered to focus only, Focus/Blur input cancelling capture.
- Tab/Shift+Tab default focus movement and `prevent_default` suppression.
- `InputPump`: per-tick budget, move coalescing, dropped counters, `request_frame` wake with a
  `ManualClock`, queue overflow, and reset (`tests/unit/input_pipeline_tests.cpp`).

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
