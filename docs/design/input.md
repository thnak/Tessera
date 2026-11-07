# Input handler and event pipeline

Status: design for Big Step B2. Companion to [`game-loop.md`](game-loop.md) and
[`scene-graph.md`](scene-graph.md).

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
| Gamepad | later | `GamepadEvent` |

Platform backends (`hal/input`) translate OS messages:

- Windows: `WM_POINTER*`, `WM_MOUSE*`, `WM_KEY*`, `WM_CHAR`, `WM_IME_*`, DPI change.
- Linux/X11: XInput2, `XKB`, RandR, Present.
- Linux/Wayland: `wl_pointer`, `wl_keyboard`, `wl_touch`, `text-input`, `wp_presentation`.
- Android: later (`MotionEvent`, `KeyEvent`, `InputConnection`).
- macOS: later (ADR-010).

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
   -> lock-free SPSC queue (or mutex queue in V1)
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

## 6. Focus and keyboard

- `focusable` is a node flag (buttons, inputs, custom nodes).
- Focus order: explicit `tab_index`, then tree order; negative tab index skips tab navigation.
- `:focus` is set on focus; `:focus-visible` is set when focus came from keyboard navigation or
  an explicit accessibility request.
- Focus trapping for modal dialogs; escape restores the previous focus.
- Window blur clears active/pressed state but may retain focus for restore.
- Keyboard events go to the focused node; if unhandled, bubble to ancestors/window.
- Shortcuts are matched after normal propagation (or before, if registered as global).
- Key repeat is delivered with `repeat=true`.
- Text input: `TextEvent` is delivered to the focused editable node; IME composition events
  update a composition buffer and are committed as text on end.

## 7. Pointer state and pseudo-classes

State machine per pointer:

```text
Idle -> Hover(target) -> Pressed(target, capture) -> Released(target)
```

- `:hover` is set on the deepest hit target (and optionally ancestors for `:hover` chains).
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
- Focus traversal, tab order, focus-visible, focus trap restore.
- Pointer capture across move/up/cancel; multi-touch.
- Hover/active state changes and resulting style invalidation.
- Keyboard repeat, shortcuts, text input, IME composition.
- Deterministic replay: identical event script -> identical state and pixels.
- Queue overflow and window blur/cancel behavior.

## 13. Platform notes

- Windows: per-monitor DPI awareness; pointer messages are preferred over legacy mouse messages;
  IME via `WM_IME_*`; focus via `WM_SETFOCUS`/`WM_KILLFOCUS`.
- X11: XInput2 for raw/pointer events; XKB for keyboard layout; RandR for DPI/refresh.
- Wayland: seat/keyboard/pointer/touch; text-input protocol; frame callbacks are not input.
- Android/macOS: deferred; keep the normalized model platform-neutral.

## 14. Open questions

- Gesture recognition (pinch, pan, long-press) and whether it belongs in `ui/` or `hal/`.
- Accessibility actions (invoke, set value) mapped into the same event flow.
- Gamepad and stylus pressure/tilt.
- IME candidate-window styling and positioning across platforms.
- Event timestamp source and clock correlation with the game loop.
