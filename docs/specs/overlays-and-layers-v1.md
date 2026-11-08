# Overlays and layers v1

Status: proposed (Big Step B2/C). Decision: ADR-029. Companion to
[`component-model-v1.md`](component-model-v1.md), [`input.md`](../design/input.md),
[`accessibility-v1.md`](accessibility-v1.md), and [`navigation-v1.md`](navigation-v1.md).

Real apps need content above the base tree: dialogs, menus, popovers, tooltips, toasts. Portals
are rejected in V1; this spec defines an explicit overlay/layer system instead.

## 1. Layers

| Layer | Use | Modal | Focus |
|---|---|---|---|
| base | app content | no | normal |
| overlay | non-modal panels/popovers | no | optional |
| modal | dialogs/sheets | yes | trapped |
| tooltip | transient hint | no | none |
| toast | transient status | no | none |

- Layers are ordered per `OverlayHost` (window/root); higher layers paint above lower.
- Tooltip/toast never take focus; modal background is inert.
- Z-order is explicit, not CSS `z-index` on arbitrary siblings.
- A layer may host a custom element (3D/popover content) but does not create a new window.

## 2. API

```cpp
struct OverlaySpec {
    std::string id;
    OverlayKind kind;
    Component content;
    Anchor anchor;              // rect/ui_id/window
    Placement placement;        // top/bottom/start/end/center
    bool dismiss_on_outside;
    bool dismiss_on_escape;
    bool modal;
    FocusPolicy focus;
    int priority;
    AnimationSpec animation;
};

OverlayHandle open_overlay(OverlaySpec);
void close_overlay(OverlayHandle);
void close_overlays(Predicate);
```

- `OverlayHost` exists per window/root; a default root host is created if none is declared.
- `open_overlay` is a runtime service; the content is a component/subtree, not a portal.
- The returned handle is stable for the overlay lifetime; callbacks are non-owning ids.
- Opening/closing is serialized on the UI thread at a frame boundary.
- Overlay content receives normal state/props; no hidden global state.

## 3. Positioning

- Anchor: node/ui id, rect, or window; track anchor layout/scroll/resize.
- Placement: top/bottom/start/end/center, with flip/shift for viewport/safe-area overflow.
- Clamp to window safe area, work area, and IME inset; never cover the caret/IME.
- Scroll tracking updates position at frame boundaries; no layout in the pointer handler.
- Virtual keyboard/insets change placement; recompute and clamp.
- Anchor removed -> overlay closes or re-anchors by policy; default close.

## 4. Modal and focus

- Modal: background inert (pointer/keyboard/accessibility), focus trap, restore previous focus.
- Escape/back dismiss policy is explicit; destructive confirmation is separate.
- Click-outside dismissal is opt-in; menus/popovers default on, dialogs default off.
- Stack: topmost modal owns focus; closing reveals the previous modal.
- Route change/window blur/background closes transient overlays by policy; modals may persist.
- Scroll lock is opt-in; body/base scroll is not mutated globally.

## 5. Lifecycle and rendering

- Open: mount content, measure, position, focus/animate, mark layer dirty.
- Close: cancel animations/async, restore focus, detach/retire nodes, mark dirty.
- Route change: close non-persistent overlays; modal routes are navigation concerns.
- Rendering: overlay layers clip to the window; shadow/blur effects obey budgets.
- No separate OS window unless an explicit `WindowSpec` is used (out of scope here).
- Overlay content can use `Scroll`, `TextInput`, `CustomElement`, etc.

## 6. Accessibility

- Overlay exposes role/name/description; modal is announced as dialog.
- Focus trap + initial focus + restore; background is hidden from the semantics tree.
- Escape/back and dismiss actions are exposed; toasts use live regions, not focus.
- Tooltips are associated with their trigger and not the only source of information.
- Overlay close/removal announces where appropriate; no focus loss to nowhere.

## 7. Security and safety

- Overlay content is app-owned; never render untrusted HTML/JS.
- Validate anchors/ids; no arbitrary window handles.
- Destructive actions require explicit confirmation, not click-outside.
- Async overlay content is cancellable on close; no callbacks after teardown.

## 8. Tests

- Layer order, open/close, stacking, handle lifetime.
- Anchor/flip/shift/clamp, resize/scroll/IME, safe area, multi-window.
- Modal focus trap/restore, click-outside, escape/back, route change.
- Overflow/clipping, shadow budgets, reduced motion.
- Accessibility snapshots/announcements; headless open/close and focus.
- No leaked nodes/handles after close; close during async load.

## 9. Open questions

- Multiple overlay hosts and cross-window overlays.
- Nested overlay stacks and focus ownership.
- Overlay transitions with navigation.
- Whether toasts/dialogs are framework primitives or library components.
- Virtualized overlay lists and large menus.
