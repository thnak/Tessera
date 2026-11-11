# Click synthesis

Status: implemented (issue #6). Code: `include/tessera/ui/scene/click_synthesis.hpp`,
`src/ui/scene/click_synthesis.cpp`, `src/ui/scene/input_dispatch_click.cpp`; tests
`tests/unit/click_synthesis_tests.cpp`.

## 1. Problem

JSX `onClick` used to map to `PointerUp`. That fires on press-A-release-B (the capture target gets
the release), after a drag that left the element, for a touch pan that started on a button, never
for the keyboard, and never for assistive technology. `onClick` now maps to a synthesized
`EventType::Click`; `onPointerUp` stays the raw release.

## 2. Rules

The `InputRouter` owns a `ClickSynthesizer` (bounded press table, pure rules) and dispatches the
event.

| Source | Fires when | Does not fire when |
|---|---|---|
| Pointer | primary pointer, left button or touch/pen contact, press then release in the scene, no slop excursion | press cancelled, window blur or focus change, release outside the scene, right/middle button, non-primary pointer |
| Keyboard | Enter key-down (not auto-repeat), or Space key-up after its own key-down, on a focused `Button` (element kind `Button` or role `button`) | key event consumed (`handled`/`prevent_default`), Ctrl/Alt/Meta held, focus moved between Space down and up, focused node is not a button |
| Accessibility | `InputRouter::invoke(root, node)`, called by the host when the a11y bridge drains a `Press` action | n/a |

Pointer clicks fire on the **nearest common ancestor** of the pressed node and the node under the
release point (the DOM rule). Press on a button label and release on the button clicks the button;
press A and release B clicks only their shared parent, so neither button handler runs.

Slop: a touch contact that travels more than `kTouchSlop` (10 px) and a pen more than `kPenSlop`
(6 px) from the press point is a pan, not a click, even if it ends on the same node. A mouse has no
slop (release on the same element still clicks after a drag), matching the DOM.

## 3. Event and delivery

`Event::click_source` is `Pointer`, `Keyboard` or `Accessibility`; `ClickEvent::source` carries it to
handlers. Keyboard and accessibility clicks use the node center and the left button, as the
`ClickEvent` schema documents (`pointerType` is `keyboard` or `assistive`).

`Click` bubbles, and its generated bindings run on every node of the bubble path, not only the
target (other event types keep Target-only bindings). This is what makes `onClick` on a Button fire
when the pressed node is its label child.

## 4. Not covered

- Double click and context menu (`onDoubleClick`, `onContextMenu`).
- Wiring the UIA and AT-SPI bridges to `invoke()`: the hosts call it from their action handler.
- Cancelling a click when a gesture claim scrolls with a mouse (touch is covered by slop).
