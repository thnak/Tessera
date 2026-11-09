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
implemented in `src/platform/windows/`. Linux/X11 and other platform translation,
platform-specific `:focus-visible` refinements (see section 6), window-blur focus retention policy
(blur already cancels captures, hover, and active), ancestor hover chains and hover recomputation on
scene changes, IME producers beyond Win32, gesture handlers, and recording remain design-only.
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
  previous pointer event, pressure is 0.5 while any button is down, and `pointer_kind` stays the
  default `Mouse`. A press captures the mouse (`SetCapture`) so the move and release arrive outside
  the window; losing the capture, focus
  or modal mode emits `PointerCancel` and clears the pressed buttons. A release without a seen
  press is ignored. Shift and Control come from the message, Alt and Meta from the keyboard state.
- **Touch and pen (`WM_POINTER*`, implemented):** `WM_POINTERDOWN`/`WM_POINTERUPDATE`/
  `WM_POINTERUP` map to pointer down/move/up for `PT_TOUCH` (`PointerKind::Touch`) and `PT_PEN`
  (`PointerKind::Pen`); `PT_MOUSE` maps to `Mouse`, and `WM_POINTERCAPTURECHANGED` or a
  `POINTER_FLAG_CANCELED` flag maps to `PointerCancel` (Windows has no `WM_POINTERCANCEL`
  message). `pointer_id` is `GET_POINTERID_WPARAM` (a Windows pointer id, used verbatim and not
  reused across kinds); `is_primary` is `IS_POINTER_PRIMARY_WPARAM`; pen pressure comes from
  `GetPointerPenInfo().pressure`, normalized and clamped to 0..1, and is 0 for touch and mouse.
  Coordinates come from `GetPointerInfo().ptPixelLocation`, converted screen->client and divided
  by the same DPI scale as the mouse path. Touch updates without contact are dropped, so touch
  never produces hover; pen in-range updates do produce `PointerMove` with `PointerKind::Pen`. The
  window does not call `EnableMouseInPointer`, so the legacy mouse path remains the only mouse
  producer, and handling pointer messages stops Windows from promoting contacts to mouse messages.
- **`WM_TOUCH` fallback (implemented):** legacy touch messages (for example after
  `RegisterTouchWindow`) translate each `TOUCHINPUT`: `TOUCHEVENTF_DOWN`/`UP`/`MOVE` map to the
  event type, `TOUCHEVENTF_PRIMARY` sets `is_primary`, and `dwID` maps to
  `0x80000000 | (dwID & 0x7fffffff)` (disjoint from WM_POINTER ids, stable per contact). Up to 32
  contacts per message are translated (bounded buffer; extras dropped, no allocation).
- **Wheel:** vertical and horizontal, in notches (120 units) with the web sign: positive `dy`
  scrolls content down, positive `dx` scrolls right; screen coordinates become client pixels.
- **Keys:** `KeyDown`/`KeyUp` with a logical code from `include/tessera/ui/key_code.hpp`
  (upper-case ASCII for letters and digits, `0x100`+ for non-printable keys). Auto-repeat sets
  `repeat` on key down. System keys (Alt combinations, F10) are recorded and still reach the
  default window procedure.
- **Text:** `WM_CHAR` becomes `Text` with the code point; surrogate pairs are combined, an orphan
  half is dropped, and control characters (including Backspace, Enter and Tab) are left to key
  events. Keyboard text stays a single-codepoint `Text` event.
- **IME (`WM_IME_*`, implemented):** start/end composition emit `CompositionStart`/`CompositionEnd`;
  `WM_IME_COMPOSITION` reads `GCS_COMPSTR`/`GCS_RESULTSTR`/`GCS_CURSORPOS` with
  `ImmGetCompositionStringW` and emits `CompositionUpdate` and/or committed `EditText` (result
  first). Payloads are bounded UTF-8 (`kMaxEditTextBytes` = 256, `text_truncated`) with UTF-16
  code-unit selection offsets and at most 512 UTF-16 units read per string. Messages are returned
  handled, so there is no default insertion; candidate-window styling is deferred (`WM_CHAR` still
  produces `Text`).
- **Focus:** `WM_SETFOCUS`/`WM_KILLFOCUS` also emit `Focus`/`Blur` input events.
- **Waking:** `wait_for_events` returns `true` while the queue holds unconsumed input, because
  input never becomes a `WindowEvent`; this is what lets a render-on-demand loop sleep until a
  pointer or key event.
- **Not yet:** dead keys, per-key `code` (physical position), OS-level pointer
  capture, IME candidate-window styling/positioning, and pen barrel/tilt/orientation beyond
  pressure.

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

Implemented subset: `InputRouter` owns a bounded capture map (at most
`InputRouter::kMaxPointerCaptures` = 8 simultaneous pointers, fixed array, no allocation). A
`PointerDown` implicitly captures the hit target for that pointer;
`set_pointer_capture(node, pointer_id)` / `release_pointer_capture(pointer_id)` expose the same
state to custom elements and tests, and `captured(pointer_id)` reports the resolved target.
Capture targets are stored as `{pointer_id, NodeId}` and resolved under the last dispatched scene
root on every use, so removing (or destroying) the captured subtree drops the capture instead of
leaving a dangling pointer behind. `PointerMove`/`PointerUp`/`PointerCancel` route to that
pointer's capture target, else the hit target; releasing or cancelling one pointer never affects
another, while a window `Focus`/`Blur` input event clears every capture. A press beyond the bounded
map is still dispatched but is not captured; an explicit release frees a slot again. Wheel events
are hit-tested at the cursor and are not delivered to the capture target; text events go to the
focused node; `pointer-events: none`, clipping, and `prevent_default` for scrolling/selection are
not implemented. `prevent_default` is honored for the Tab default action only (section 6). Platform
touch/pen translation and gesture recognizers are still missing, so independent per-pointer
captures are exercised by synthetic events for now.

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
  `Style` dirty through `set_pseudo_state`, and `ui/style/scene_style.hpp` exposes the bit to
  selector matching, so `:focus-visible` rules re-resolve on the next style pass.
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
- Keyboard shortcuts/global handlers, editable-node text editing/selection, and IME composition.
  Committed characters already reach the focused node as `Text` events, but there is no editable
  node model behind them yet.

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

- The primary pointer is the pointer that went down while no other pointer was captured; it is
  pointer 0 before the first press. When it goes up or is cancelled, the oldest remaining capture
  (insertion order) becomes primary, or pointer 0 when none remain. Only the primary pointer drives
  `Hover`/`Active`; secondary presses keep their own capture but do not steal hover or active.
- `:hover`: `InputRouter::hovered()` is the deepest hit target of the primary pointer's last
  `PointerMove`. A new hover target gains `PseudoState::Hover` and the previous hover target loses
  the bit on the next move; a move that hits nothing (the pointer left the scene) clears it. A
  captured pointer freezes the hover chain (see the boundary-event rule below). Only `PointerMove`
  updates hover: a synthetic `PointerDown` with no preceding move activates but does not hover,
  moves from non-primary pointers are ignored for hover, and touch moves never hover at all.
- Hover boundary events (implemented): a hover-target change fires the W3C pointer family.
  `PointerOut` is dispatched at the old target and bubbles through its ancestors down to
  (excluding) the deepest common ancestor of the old and new chains; `PointerLeave` then fires once
  on each old-chain node strictly below that ancestor, innermost to outermost. `PointerOver` and
  `PointerEnter` mirror this for the new chain, with `PointerEnter` firing outermost to innermost.
  Enter/leave do not bubble; over/out bubble. The events carry the current pointer payload
  (id/kind/position/pressure/timestamp). A null side (entering or leaving the scene) makes the
  common ancestor empty, so leaving fires out/leave on the whole old chain including the root and
  entering fires over/enter from the root down.
- Capture freezes boundary events: while a pointer is captured its moves still dispatch to the
  capture target, but they do not update `hovered()`, the `Hover` bits, or fire
  over/out/enter/leave. The chain resumes on the first move after the capture is released. A window
  `Blur`, a primary mouse/pen `PointerCancel`, and `InputRouter::reset()` (silently) clear the
  chain; cancel/blur fire out/leave so no stale enter state remains. Touch never fires boundary
  events.
- Only the deepest hit target carries `Hover`; ancestors are not hovered. Ancestor `:hover` chains
  are deferred together with scene-change recomputation: a chain must be recomputed when the tree,
  bounds, or visibility change, and the input layer has no change notification yet. Maintaining it
  from pointer moves alone would leave stale ancestor bits.
- `:active`: `InputRouter::active()` is the primary pointer's `PointerDown` hit target. The bit is
  cleared before the primary pointer's `PointerUp` event is dispatched, and also on its
  `PointerCancel`, a window `Focus`/`Blur` input event, and `InputRouter::reset()`. A primary press
  that hits nothing clears any older active target. Capture does not move `Active`, and a secondary
  pointer press never sets it.
- `:focus` is independent of pointer state; `FocusManager` owns the `PseudoState::Focus` bit and
  clears/sets it around `Blur`/`Focus` dispatch. Pointer-down focus behavior is unchanged: every
  `PointerDown` (primary or secondary) follows the same focus rules as before.
- Window `Focus`/`Blur` input events reset interaction state: all captures, the hover chain
  (firing out/leave), and active are cleared because no pointer can be considered over the scene.
  Scene focus is retained (see section 6); `FocusManager` remains the owner of scene-level
  Focus/Blur events.
- `:disabled`/`:checked` come from node state, not pointer state.
- State changes go through `Node::set_pseudo_state`, which marks `Dirty::Style`; the style resolver
  then raises layout/paint/render dirty domains only if the matched rules change those groups. A
  repeated move onto the same target does not churn the dirty flags. `SceneStyleTree::compute`
  refreshes the cached pseudo-state from the node on each evaluation, so a hover/active change is
  reflected by the next style resolution without rebuilding the tree.
- Hover and active targets are also validated by `NodeId` before their bit is cleared, so a target
  removed from the scene is never dereferenced; the interaction target is dropped instead.
  `hovered()` and `active()` remain raw scene pointers for observation, so reset or destroy the
  router before destroying the scene root.

Text/IME routing (implemented, plumbing only): `Text` (one codepoint) and the IME family
(`CompositionStart`/`Update`/`End`, `EditText`) go to the focused node through the normal path as
the matching scene `EventType`; without focus they are dropped. The inline UTF-8 payload is capped at
`kMaxEditTextBytes` (256 bytes) with `text_truncated`; selection offsets are UTF-16 code units.
`prevent_default` is advisory (no framework text insertion yet).

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
  offsets are UTF-16 code units as the platform reports them.
- Win32 reads `GCS_COMPSTR`/`GCS_RESULTSTR`/`GCS_CURSORPOS` bounded to 512 UTF-16 units and returns
  the messages handled, so there is no default IME insertion.

Not implemented: editable nodes (`TextInputKind`), candidate-window positioning/styles, password
handling, and IME producers beyond Win32.

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
  clearing hover/active.
- Tab/Shift+Tab default focus movement and `prevent_default` suppression.
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

## 13. Platform notes

- Windows: per-monitor DPI awareness; pointer messages are preferred over legacy mouse messages;
  IME via `WM_IME_*`; focus via `WM_SETFOCUS`/`WM_KILLFOCUS`.
- X11: XInput2 for raw/pointer events; XKB for keyboard layout; RandR for DPI/refresh.
- Wayland: seat/keyboard/pointer/touch; text-input protocol; frame callbacks are not input.
- Android/macOS: deferred (Android: Big Step F; macOS: ADR-010); keep the normalized model platform-neutral.
- Resize/DPI/window lifecycle is normalized separately; see
  [`window-resize.md`](window-resize.md).

## 14. Open questions

- Gesture recognizers/arena (the system that decides which recognizer wins a pointer sequence) are
  implemented as a pure, scene-independent state machine in `include/tessera/ui/gesture.hpp`
  (bounded per-pointer slots, claim/reject protocol, tap/double-tap/long-press/drag/swipe/pinch/
  rotate recognizers); see [`scroll-and-gestures-v1.md`](../specs/scroll-and-gestures-v1.md)
  section 5.1 and the pan-scroll handoff in section 5.2. Remaining: non-scroll gesture handling,
  custom-element consumption, wheel/edge-swipe recognizers, and platform edge/back gesture policy.
- Accessibility actions (invoke, set value) mapped into the same event flow (see [`accessibility.md`](accessibility.md)).
- Gamepad and stylus pressure/tilt.
- IME candidate-window styling and positioning across platforms.
- Event timestamp source and clock correlation with the game loop.
