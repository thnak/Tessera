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
`src/ui/scene/input_dispatch.cpp` and `include/tessera/ui/scene/event.hpp`. Bounded per-pointer
capture with explicit capture APIs, primary-pointer hover/active tracking, the window-focus
interaction reset, the `:focus-visible` pointer-versus-keyboard heuristic, and the
mouse/touch/pen pointer-kind model (hover is mouse/pen only, touch events only activate) are
implemented in
`src/ui/scene/input_dispatch.cpp`, `src/ui/scene/focus.cpp`,
`include/tessera/ui/scene/focus.hpp`, and `include/tessera/ui/input.hpp` (sections 4-7). The
per-tick input pump
(queue -> sampler -> router -> `RuntimeLoop::request_frame`) is
`include/tessera/ui/input_pump.hpp` and `src/ui/input/input_pump.cpp`. Windows platform translation
(mouse, touch/pen, IME, wheel, key and character messages into `InputEvent`, see section 2.1) is
implemented in `src/platform/windows/`; Linux/X11 translates core input plus XInput2 smooth scroll,
touch, pen and XIM pre-edit (sections 2.2 and 8). Other platform translation, platform-specific
`:focus-visible` refinements (see section 6), window-blur focus retention policy (blur already
cancels captures, hover, and active), ancestor hover chains and hover recomputation on scene
changes, IME producers beyond Win32 and X11, gesture handlers, and recording remain design-only.
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
| Pointer | mouse move/down/up, pen | `PointerEvent` (`pointer_kind` Mouse/Pen) |
| Wheel | scroll, trackpad | `WheelEvent` |
| Touch | down/move/up/cancel, multi-touch | `PointerEvent` with pointer id, `pointer_kind` Touch |
| Keyboard | key down/up, repeat, dead keys | `KeyEvent` |
| Text | committed characters | `TextEvent` |
| IME | composition start/update/end | `CompositionEvent` |
| Window | focus/blur, enter/leave, resize, DPI | lifecycle/focus events |
| Gamepad | not in B2; no release named (see section 14) | `GamepadEvent` |
| Custom | element-emitted | `CustomEvent` |

Platform backends (`hal/input`) translate OS messages:

- Windows: `WM_POINTER*`, `WM_MOUSE*`, `WM_KEY*`, `WM_CHAR`, `WM_IME_*`, DPI change.
- Linux/X11: core pointer/key events plus XIM text, and with XInput2 (section 2.2) smooth-scroll
  valuators, touch sequences and pen/tablet pressure; RandR and Present planned.
- Linux/Wayland: `wl_pointer`, `wl_keyboard`, `wl_touch`, `text-input`, `wp_presentation`.
- Android: not implemented; planned in Big Step F (`MotionEvent`, `KeyEvent`, `InputConnection`; see [`mobile-integration.md`](mobile-integration.md)).
- macOS: deferred ([ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md), macOS via MoltenVK).

No platform-specific type may leak into `ui/`.

### 2.1 Windows translation (implemented)

`hal::Window::set_input_queue(ui::InputQueue*)` routes translated input into the application's
queue (not owned; reset it with `nullptr` before destroying it). `Win32InputTranslator` runs on the
window's thread, so the queue's single producer is the UI thread.

- **Pointer:** `WM_MOUSEMOVE` and the left/right/middle/X1/X2 buttons, in logical pixels (physical
  position / DPI scale) with `dx`/`dy` from the previous event, pressure 0.5 while any button is
  down, and `pointer_kind` `Mouse`. A press captures the mouse (`SetCapture`) so move and release
  arrive outside the window; losing the capture, focus or modal mode emits `PointerCancel`. A
  release without a seen press is ignored; Alt/Meta come from the keyboard state.
- **Touch and pen (`WM_POINTER*`):** down/update/up for `PT_TOUCH` (`Touch`) and `PT_PEN` (`Pen`);
  `PT_MOUSE` maps to `Mouse`, `WM_POINTERCAPTURECHANGED`/`POINTER_FLAG_CANCELED` to
  `PointerCancel`. `pointer_id` is `GET_POINTERID_WPARAM`, `is_primary` is
  `IS_POINTER_PRIMARY_WPARAM`, pen pressure is `GetPointerPenInfo().pressure` clamped to 0..1 (0
  for touch/mouse). Touch updates without contact are dropped (no touch hover); pen in-range
  updates produce `PointerMove`. `EnableMouseInPointer` is not called, so the legacy mouse path
  stays the only mouse producer.
- **`WM_TOUCH` fallback:** each `TOUCHINPUT` maps its flags to down/up/move, `TOUCHEVENTF_PRIMARY`
  to `is_primary`, and `dwID` to `0x80000000 | (dwID & 0x7fffffff)` (disjoint from WM_POINTER ids);
  up to 32 contacts per message (bounded, extras dropped).
- **Wheel, keys and text:** wheel notches (120 units) with the web sign (positive `dy` scrolls
  content down, positive `dx` right). `KeyDown`/`KeyUp` carry a logical code from
  `include/tessera/ui/key_code.hpp`; auto-repeat sets `repeat`, and system keys (Alt combinations,
  F10) still reach the default procedure. `WM_CHAR` becomes one `Text` per code point (surrogates
  combined, orphan halves dropped, control characters left to key events).
- **IME (`WM_IME_*`):** start/end emit `CompositionStart`/`CompositionEnd`;
  `WM_IME_COMPOSITION` reads `GCS_COMPSTR`/`GCS_RESULTSTR`/`GCS_CURSORPOS` and emits
  `CompositionUpdate` and/or committed `EditText` (result first), bounded UTF-8 with UTF-16
  code-unit offsets and at most 512 units per string; messages are returned handled. Candidate
  windows are deferred.
- **Focus and waking:** `WM_SETFOCUS`/`WM_KILLFOCUS` also emit `Focus`/`Blur`; `wait_for_events`
  returns true while the queue holds input, which is what lets a render-on-demand loop sleep.
- **Not yet:** dead keys, per-key physical `code`, OS-level pointer capture, IME candidate-window
  styling/positioning, and pen tilt.

### 2.2 Linux/X11 XInput2 translation (implemented)

When the server has XInput and the build found `X11/extensions/XInput2.h` plus libXi
(`TESSERA_X11_XI2`), the window selects XI2 events and translates them with the pure rules in
`src/platform/linux/x11_xi2_translation.{hpp,cpp}`; without either, the core path above is
unchanged. The Xlib pump is `src/platform/linux/x11_xi2*.cpp`, the pre-edit state machine
`src/platform/linux/x11_ime_composition.{hpp,cpp}`, and both are unit-tested without a display
(`tests/unit/x11_input_translation_xi2_tests.cpp`). Selected are motion and buttons on
`XIAllMasterDevices`, touch begin/update/end per touch-capable slave device, and
`XI_HierarchyChanged` on `XIAllDevices`: an event delivered as XI2 stops the core event for it, so
the pump owns the pointer path while XI2 is active, while keys, focus, resize and close stay core.

- **Scroll:** XI 2.1 valuators. One scroll unit is the axis' `XIScrollClass` increment (one notch);
  raw deltas become wheel units and the per-device remainder accumulates (no drift), while a
  zero/non-finite increment disables the axis. Positive vertical means scroll down (it emulates
  button 5) and positive horizontal means right, which is the web sign, so
  `dy = raw_v / increment_v` and `dx = raw_h / increment_h`; natural scrolling is applied by the
  driver and not inverted again. The server's emulated duplicate of each notch or valuator carries
  `XIPointerEmulated` and is dropped, so one notch is one `Wheel` event. The model has no scroll
  phase: each wheel event is an independent delta.
- **Touch:** `PointerKind::Touch` from a bounded slot table keyed by `(device id, touch id)` with a
  stable `pointer_id` (`0x8000'0000` base): begin→`PointerDown`, update→`PointerMove`, end→
  `PointerUp`, device removal→`PointerCancel`. Unpaired samples and updates after an end are
  ignored, 16 contacts bound the table, and touch-emulated pointer events are dropped.
- **Pen/tablet:** `PointerKind::Pen` with pressure normalized from the pressure valuator and
  clamped to 0..1 (0.5 when the device has no pressure axis, 0 while hovering). Tilt and the
  eraser tool are classified, but the normalized event has no field for them: `PenValue` reports
  both and they are dropped at the queue boundary.
- **IME pre-edit:** with `XIMPreeditCallbacks`, `XNPreeditStart/Draw/Done` drive the bounded
  composition: start→`CompositionStart`, draw→`CompositionUpdate` (the XIM draw is a splice), done
  →`EditText` + `CompositionEnd`. Payloads clamp at `kMaxEditTextBytes` on a UTF-8 boundary,
  malformed bytes become U+FFFD, the caret is a UTF-8 byte offset converted from the XIM character
  index, and the duplicate keycode-0 commit is dropped by a commit gate. A wide-character pre-edit
  is treated as empty (UTF-8 locales use the multibyte form), and no text editor paints the marked
  range yet.

## 3. Event model

```cpp
enum class PointerButton { Left, Right, Middle, Back, Forward, Other };
enum class PointerKind { Mouse, Touch, Pen }; // device kind; defaults to Mouse
enum class KeyCode { /* logical, layout-independent */ };
enum class Modifier { Shift, Control, Alt, Meta, CapsLock, NumLock };

struct InputEvent {
    EventType type;
    double timestamp_s;      // simulated clock in deterministic mode
    std::uint32_t device_id;
    std::uint32_t pointer_id;
    PointerKind pointer_kind;
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
event types plus the hover-boundary types `PointerOver`, `PointerOut`, `PointerEnter`, and
`PointerLeave`, and carries the payload of the normalized event: `button`, `pointer_id`,
`pointer_kind`, `pressure`, `scroll_x`/`scroll_y` (web sign), `timestamp_s`, `is_primary`, and
`prevent_default`. `PointerOver`/`PointerOut` bubble (like `mouseover`/`mouseout`), while
`PointerEnter`/`PointerLeave` do not bubble and fire once per node on the entered/left chain
(section 7). `EditText` carries committed multi-codepoint text and `CompositionStart`/`Update`/`End`
carry the in-progress composition; both use the bounded inline UTF-8 payload (at most
`kMaxEditTextBytes` = 256 bytes, `text_truncated` when cut) plus a UTF-16 code-unit selection
range. `handled` stays advisory; `prevent_default` is read by the dispatcher after propagation and
currently suppresses only the Tab focus default action (it is advisory/no-op for text events, which
have no framework default insertion yet). Pointer and wheel fields are zero (mouse kind) for key,
text, IME, and focus events.

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
  Coalescing is keyed by `(pointer_id, pointer_kind)`: moves of different device kinds are never
  merged even if a producer reuses an id, so the router always sees the kind the input came from.
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

Optional gesture pipeline (implemented, additive): `enable_gestures(config)` registers the built-in
gesture recognizers on an owned `GestureArena`, and every pumped event is then fed to the arena
exactly once, in queue order, right after the router dispatched it. Pointer dispatch, capture, and
focus are unchanged; a gesture claim never suppresses a pointer event in v1. The gesture pan
scroll handoff (section 5.2) only changes offsets; `InputDispatchResult::target_id` carries
the stable `NodeId` of the routed target, and `GestureEvent::target_id` reports the
survives relayout/removal). `InputPump::tick(timestamp_s)` drives time-based recognizers from the
frame path with sampled loop time (no wall clock). Gesture events go to a caller-installed bounded
`InputPump::GestureCallback` (function pointer + user data, called synchronously on the pumping
thread); the arena's fixed 32-event sink bounds one pump/tick call. See
[`scroll-and-gestures-v1.md`](../specs/scroll-and-gestures-v1.md) section 5.1.

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

- `set_pointer_capture(node, pointer_id)` (implicitly on pointer down); all subsequent
  move/up/cancel for that pointer go to the capture target and its ancestors, regardless of hit
  testing.
- Capture is released on up/cancel or explicit `release_pointer_capture(pointer_id)`.
- Capture is per pointer id; independent pointers hold independent captures (implemented as a
  bounded fixed-capacity map).
- Losing the window/focus cancels every capture, and clears hover/active states.

Implemented subset: `InputRouter` owns a bounded capture map (`kMaxPointerCaptures` = 8 fixed
slots, no allocation). A `PointerDown` implicitly captures the hit target;
`set_pointer_capture`/`release_pointer_capture` expose the same state to custom elements and tests,
and `captured(pointer_id)` reports the resolved target. Targets are stored as `{pointer_id, NodeId}`
and resolved under the last dispatched root, so a removed subtree drops its capture instead of
dangling. `PointerMove`/`Up`/`Cancel` route to that pointer's capture target, else the hit target;
releases and cancels are per pointer, and a window `Focus`/`Blur` clears every capture. Wheel events
are hit-tested at the cursor and not sent to the capture target; text goes to the focused node.
`pointer-events: none`, clipping, and `prevent_default` for scrolling/selection are not implemented
(`prevent_default` is honored for the Tab action only, section 6).

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
- `:focus-visible` (implemented heuristic): `FocusManager` also owns the
  `PseudoState::FocusVisible` bit. `request_focus` clears it on the node it focuses and on the
  previous node, because a programmatic request has no input modality. The `InputRouter` applies
  the modality per input event: a pointer press that focuses a node clears the bit, and the
  Tab/Shift+Tab default action sets it on the resulting focus target. The modality of the causing
  input wins when a `Blur` handler redirects focus during the request: keyboard leaves the ring
  set, pointer leaves it clear. `clear_focus`/`reset` clear both bits. A caller that wants the ring
  after a programmatic `request_focus` must call `FocusManager::set_focus_visible(true)`
  explicitly; direct `focus_next`/`focus_previous` calls count as programmatic. The change marks
  `Style` dirty, so `:focus-visible` rules re-resolve on the next style pass.
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
- `KeyDown` of Tab is a default action: after propagation it runs `focus_next` (or
  `focus_previous` with Shift) unless a handler set `prevent_default`. It runs even with no current
  focus, so the first Tab focuses the first candidate (Shift+Tab the last) and a modal trap limits
  the traversal. `InputDispatchResult` reports `default_prevented` and `focus_moved`.
- `PointerDown` on a focusable hit target requests focus; a pointer down on a non-focusable target
  inside the trap (or anywhere without an active trap) clears focus. Pointer down outside an
  active trap neither steals nor clears focus. Pointer capture remains independent and is unchanged
  by focus changes.
- Default action (implemented): after a `KeyDown` with `ui::key::kTab` propagates, the router calls
  `focus_next(root)`, or `focus_previous(root)` when Shift is held, unless a handler set
  `Event::prevent_default`. The modal trap is enforced inside `FocusManager`.
  `InputDispatchResult` reports `default_prevented` and `focus_moved`. `KeyUp` for Tab has no
  default action. `Text` input events are delivered to the focused node as `EventType::Text`.
- Window focus inputs (`Focus`/`Blur`) cancel every pointer capture and clear hover/active
  interaction state (section 7); they do not clear scene focus, because `FocusManager` remains the
  owner of the scene-level Focus/Blur events.

Deferred policy (design intent, not implemented):

- Platform-specific `:focus-visible` refinements (text-input/IME focus, keyboard shortcuts that
  focus a node) and window-blur focus retention.
- Keyboard shortcuts/global handlers and editable-node text editing/selection: committed text and
  composition events reach the focused node, but no editable node model consumes them yet.

## 7. Pointer state and pseudo-classes

Implemented state machine per pointer (bounded capture map; platform touch/gestures deferred):

```text
Idle -> Hover(target) -> Pressed(target, capture) -> Released(target)
```

Capture rules (implemented):

- State is keyed by `InputEvent::pointer_id` in a fixed-capacity array (at most
  `InputRouter::kMaxPointerCaptures` = 8), so the hot path never allocates. A press beyond the bound
  is dispatched but not captured; an explicit release frees the slot again.
- A `PointerDown` implicitly captures its hit target for that pointer.
  `set_pointer_capture(node, pointer_id)` and `release_pointer_capture(pointer_id)` expose the same
  state to custom elements and tests. Explicit capture neither dispatches an event nor changes the
  primary pointer.
- `PointerMove`/`PointerUp`/`PointerCancel` route to that pointer's capture target, else the hit
  target. Removing one pointer never affects another. `PointerCancel` ends only the cancelled
  pointer (that is what the platform sends when one touch/device sequence is cancelled); a window
  `Focus`/`Blur` input event clears every capture.
- Capture targets are stored as `{pointer_id, NodeId}` and resolved under the last dispatched scene
  root on every use. If the captured subtree was removed or destroyed, the entry is pruned and the
  event falls back to hit testing, so a stale capture cannot be dereferenced.
- The single-pointer `captured()` view returns the primary pointer's capture target.

Pointer kinds (implemented):

- `InputEvent::pointer_kind` (`Mouse`/`Touch`/`Pen`, default `Mouse`) is mirrored on
  `scene::Event` so handlers can branch without changing the payload contract; the default keeps
  every pre-kind producer and test byte-identical in behavior.
- Hover is generated by mouse and pen only; a touch contact never hovers, because a contact has no
  pre-contact position to hover with. Pen is included because pens report hover positions while
  out of contact. Touch/pen events never modify hover state, so a hover set by an earlier
  mouse/pen move remains until the next mouse/pen move (it is not touch residue).
- A touch/pen `PointerDown` still becomes `Active` and is captured exactly like a mouse press; its
  `PointerUp`/`PointerCancel` clears `Active` and never leaves `Hover` on the press target (no
  touch/pen path sets hover, so a previous mouse hover target is left untouched).
- A pen that has not pressed yet hovers only when its `pointer_id` is the primary pointer (pointer 0
  before the first press); platform translation must map a primary pen hover to that id. Secondary
  mouse/pen moves and any touch contact never move hover.
- Touch/pen focus is pointer modality for the `:focus-visible` heuristic (section 6): the ring
  stays clear after a touch/pen press focuses a node.
- Capture remains keyed by `pointer_id` only. Ids are document-scoped across kinds: producers must
  not reuse an id for two live pointers, so a mouse and a touch pointer keep separate ids; events
  for one id from different kinds would share the capture entry. The sampler never coalesces moves
  of different kinds even if an id is reused.
- Platform sources set the field where touch/pen input is translated: the Windows translator emits
  `Touch`/`Pen` from `WM_POINTER*` and `Touch` from the legacy `WM_TOUCH` fallback (id spaces in
  section 2.1), while mouse messages remain the mouse producer. Linux/X11 and Wayland touch/pen,
  macOS `NSResponder`, and Android `MotionEvent` translation are still deferred.

Hover/active (implemented, primary pointer only):

- The primary pointer is the one that went down while no other pointer was captured (pointer 0
  before the first press); when it goes up or is cancelled, the oldest remaining capture becomes
  primary, or pointer 0 when none remain. Only it drives `Hover`/`Active`; secondary presses keep
  their capture but do not steal them.
- `:hover`: `InputRouter::hovered()` is the deepest hit target of the primary pointer's last
  `PointerMove`. A new target gains `PseudoState::Hover` and the previous one loses it on the next
  move; a move that hits nothing clears it, and a captured pointer freezes the chain (below). Only
  `PointerMove` updates hover: a press with no preceding move activates but does not hover, moves
  from non-primary pointers are ignored, and touch moves never hover.
- Hover boundary events fire the W3C family: `PointerOut` at the old target bubbles to (excluding)
  the deepest common ancestor of the old and new chains, then `PointerLeave` fires once on each old
  node below it, innermost first; `PointerOver`/`PointerEnter` mirror this for the new chain, with
  enter outermost first. Enter/leave do not bubble, over/out do, and each carries the pointer
  payload. A null side leaves the whole old chain (root included) and enters from the root down.
- Capture freezes boundary events: moves still dispatch to the capture target but do not update
  `hovered()`, the `Hover` bits, or fire over/out/enter/leave; the chain resumes on the first move
  after release. A window `Blur`, a primary mouse/pen `PointerCancel` and `InputRouter::reset()`
  clear it (cancel/blur fire out/leave so no stale enter state remains); touch never fires them.
- Only the deepest target carries `Hover`; ancestor `:hover` chains and their recomputation on
  scene changes are deferred: a chain must be recomputed when the tree, bounds or visibility
  change, and the input layer has no change notification.
- `:active`: `InputRouter::active()` is the primary pointer's `PointerDown` target, cleared before
  its `PointerUp` and on its `PointerCancel`, a window `Focus`/`Blur` and `InputRouter::reset()`.
  A press that hits nothing clears any older active target; capture does not move `Active` and a
  secondary press never sets it.
- `:focus` is independent of pointer state (`FocusManager` owns the bit); a `PointerDown` on a
  focusable target requests focus, on a non-focusable target inside a trap (or anywhere without
  one) clears it, and outside a trap neither steals nor clears it.
- `:disabled`/`:checked` come from node state, not pointer state. State changes go through
  `Node::set_pseudo_state`, which marks `Dirty::Style`; the style resolver raises layout/paint/
  render only if the matched rules change those groups, and a repeated move onto the same target
  does not churn the flags (the cached pseudo-state is refreshed on each evaluation).
- Hover/active targets are validated by `NodeId` before their bit is cleared, so a removed target
  is never dereferenced (the target is dropped); `hovered()`/`active()` stay raw pointers, so reset
  or destroy the router before the scene root.

Text/IME routing (implemented, plumbing only): `Text` (one codepoint) and the IME family
(`CompositionStart`/`Update`/`End`, `EditText`) go to the focused node as the matching scene
`EventType` and are dropped without focus; the payload bounds and offsets are in section 8.

Keyboard accelerators (implemented): `InputRouter` owns a bounded `KeyBindingRegistry` of
`KeyChord` shortcuts; see [`os-events-v1.md`](../specs/os-events-v1.md) §12.1 (rules/bounds).

Not implemented: ancestor `:hover` bits and hover recomputation on scene changes (boundary events
fire on the chain, but the pseudo-state bit remains deepest-target only), OS-level pointer capture,
accessibility announcements for pointer boundaries, ancestor hover recomputation driven by layout
changes, and interaction state derived from scene mutations not driven by input (for example a
hovered node becoming invisible keeps its bit until the next move). A detached-but-not-destroyed
node keeps its hover/active bits; only an id-resolvable node is cleared.

## 8. Text input and IME

Implemented plumbing (the text editor itself is a later slice):

- `Text` is one committed codepoint from `WM_CHAR`; `EditText` is committed UTF-8 text from an IME
  result string; `CompositionStart`/`Update`/`End` mirror the IME preedit lifecycle. All are routed
  to the focused node only (section 7).
- Payload: inline UTF-8 capped at `kMaxEditTextBytes` (256 bytes) with `text_truncated`; selection
  offsets are UTF-8 byte offsets into that payload; Win32 converts `GCS_CURSORPOS` from UTF-16.
- Win32 reads `GCS_COMPSTR`/`GCS_RESULTSTR`/`GCS_CURSORPOS` bounded to 512 UTF-16 units and returns
  the messages handled, so there is no default IME insertion.

Not implemented: editable nodes (`TextInputKind`), candidate-window positioning/styles, password
handling, and IME producers beyond Win32.

## 9. Determinism and recording

- Every event carries a simulated timestamp in deterministic mode; a recorded script is a sequence
  of `(tick, event)` records and replay is exact.
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
- Debug overlay: pointer id/position/target, hover/active target, focus path, capture targets.

## 12. Tests

- Hit testing with transforms, clipping, visibility, `pointer-events: none`.
- Capture/target/bubble order; stop propagation; prevent default.
- Focus set/clear and blur-before-focus order; invalid focus rejection.
- Tab traversal order/wrap, positive/zero/negative `tab_index` ordering, hidden-subtree skipping,
  keyboard routing with stop propagation (`tests/unit/focus_tests.cpp`,
  `tests/unit/focus_policy_tests.cpp`).
- Per-pointer capture: two independent pointers with interleaved moves, releasing one pointer
  keeping the other, primary hand-off, cancel of one pointer, window blur clearing every capture,
  bounded-map overflow/slot reuse, explicit set/release capture, and a captured subtree removed
  from the scene (id-resolved, no dangling dereference) (`tests/unit/input_router_tests.cpp`); the
  same capture/blur flow through `InputPump` (`tests/unit/input_pipeline_tests.cpp`).
- Modal trapping: focus confined to the subtree for Tab and pointer input, rejected outside
  requests, saved-focus restoration, invalid-saved-focus behavior, and trap release when the modal
  root is removed (`tests/unit/focus_policy_tests.cpp`).
- `:focus`/`:not(:focus)` selector matching and style re-evaluation, including `Style` dirty
  marking (`tests/unit/focus_style_tests.cpp`).
- `:focus-visible` heuristic: pointer focus clears the bit while keeping `:focus`, Tab/Shift+Tab set
  both, blur/clear remove both, modal-trap Tab and Blur-handler redirects keep the causing
  modality, programmatic `request_focus` clears unless `set_focus_visible(true)` follows, router
  reuse across scene roots, and computed-style re-evaluation (`tests/unit/focus_tests.cpp`,
  `tests/unit/focus_policy_tests.cpp`, `tests/unit/focus_style_tests.cpp`).
- Hover boundary events: enter from empty (over bubbles, enter outermost first), same-parent move
  (out/leave old then over/enter new), ancestor move (only the child leaves), moving back down (only
  the child enters), leaving the scene (out through the chain then leave innermost first), exact
  `Target`/`Bubble` phase ordering, no events for touch, capture freezing the chain until release,
  cancel and blur cleanup, and `:hover`/`Style` dirty consistency
  (`tests/unit/input_dispatch_tests.cpp`).
- Hover/active state changes and style invalidation: hover enter, move to another target, and
  leave (previous hover cleared), hidden targets skipped, active down/up, cancel and window
  focus/blur cleanup, `Style` dirty marking, and stateless `dispatch_input` not owning state
  (`tests/unit/input_router_tests.cpp`, `tests/unit/input_dispatch_tests.cpp`); the same flow
  through `InputPump` (`tests/unit/input_pipeline_tests.cpp`).
- Pointer kinds: mouse hover/active unchanged; a touch press activates, captures and focuses but
  never hovers; touch up/cancel leaves no hover or active residue; a secondary touch pointer never
  steals hover or active; pen hover is allowed and pen pressure/kind are forwarded; `pointer_kind`
  reaches scene handlers; capture is isolated across pointer ids; touch focus keeps
  `:focus-visible` clear; and the sampler never coalesces moves of different kinds
  (`tests/unit/input_dispatch_tests.cpp`, `tests/unit/input_tests.cpp`,
  `tests/unit/focus_tests.cpp`).
- IME/text plumbing: pure Win32 translation (result vs composition strings, end, empty, UTF-8
  truncation at the inline cap, surrogate pairs, cursor clamping), and routing to the focused node
  only with dropped input when unfocused (`tests/unit/win32_ime_tests.cpp`,
  `tests/unit/input_text_tests.cpp`).
- Deterministic replay: identical event script -> identical state and pixels (not implemented).
- Queue overflow and window blur/cancel behavior.
- Scene payload mapping: button, pointer id, pointer kind, pressure, wheel deltas, timestamp
  (`tests/unit/input_pipeline_tests.cpp`).
- Wheel hit-testing/bubbling, text delivered to focus only, Focus/Blur input cancelling capture and
  clearing hover/active; Tab/Shift+Tab default focus movement and `prevent_default` suppression.
- Gesture pipeline: tap/double-tap/long-press (sampled tick time)/drag/swipe end-to-end from raw
  events, stable target id across relayout and node removal, identical W3C route log with and
  without a gesture sink installed, pointer cancel and window blur cleanup, two-pointer target
  rules, deterministic replay, and pipeline-vs-direct-arena agreement
  (`tests/unit/gesture_pipeline_tests.cpp`).
- Windows translation: mouse/wheel/key/text through the real window, plus pure pointer/touch
  mapping tests (type->kind with unknown->Mouse, primary flag, pressure clamp, pointer-id
  preservation, `WM_TOUCH` fallback ids/flags, no touch hover)
  (`tests/unit/window_win32_input_tests.cpp`).
- `InputPump`: per-tick budget, move coalescing, dropped counters, `request_frame` wake with a
  `ManualClock`, queue overflow, and reset (`tests/unit/input_pipeline_tests.cpp`).
- X11 XInput2 translation: valuator scroll at several increments and both signs, fractional
  accumulation (no drift, no double emit), disabled axes and NaN/Inf deltas; touch begin/update/end/
  cancel per slot with two interleaved contacts, unpaired samples and updates after end ignored, the
  slot bound and device-removal cancels; pen pressure/tilt normalization, the no-pressure default,
  eraser classification and dropped tilt; the XIM pre-edit lifecycle (start, updates, commit,
  cancel, empty commit, replacement start, clamped payloads, malformed UTF-8, caret byte offsets),
  and a byte-identical replay (`tests/unit/x11_input_translation_xi2_tests.cpp`, ctest
  `x11_xi2_translation`).

## 13. Platform notes

- Windows: per-monitor DPI awareness; pointer messages are preferred over legacy mouse messages;
  IME via `WM_IME_*`; focus via `WM_SETFOCUS`/`WM_KILLFOCUS`.
- X11: XInput2 for raw/pointer events; XKB for keyboard layout; RandR for DPI/refresh.
- Wayland: seat/keyboard/pointer/touch; text-input protocol; frame callbacks are not input.
- Android/macOS: deferred (Android: Big Step F; macOS: ADR-010); keep the normalized model platform-neutral.
- Resize/DPI/window lifecycle is normalized separately; see
  [`window-resize.md`](window-resize.md).

## 14. Open questions

- Gesture recognizers/arena are a pure, scene-independent state machine in
  `include/tessera/ui/gesture.hpp`; see [`scroll-and-gestures-v1.md`](../specs/scroll-and-gestures-v1.md)
  sections 5.1-5.2. Remaining: non-scroll gesture handling, custom-element consumption,
  wheel/edge-swipe recognizers, and platform edge/back gesture policy.
- Accessibility actions (invoke, set value) mapped into the same event flow (see [`accessibility.md`](accessibility.md)).
- Gamepad input, and a tilt/orientation field on the normalized event (XI2 classifies tilt, but the
  event has nowhere to carry it).
- IME candidate-window styling and positioning across platforms.
- Event timestamp source and clock correlation with the game loop.
