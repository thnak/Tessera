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

## 8.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `OverlayHost` | layer stack, z-order, host bounds, focus scope | Route stack/back semantics |
| `OverlayEntry` | spec, content instance, lifecycle, anchor tracking | Page state (app owns) |
| `OverlayService` | open/close/handles/policy validation | Rendering/scene details |
| Input/focus | focus trap/inert/escape delivery | Overlay dismissal policy |
| Navigation | modal route stack/back | Non-route dialogs/popovers |

Entry state machine:

```text
Created -> Mounting -> Measuring -> Opening -> Open -> Closing -> Closed
                         \-> Failed (content error)
```

- `Mounting` instantiates content; `Measuring` obtains intrinsic size; `Opening` positions and
  runs animation; `Open` tracks anchor; `Closing` cancels async/animation and restores focus.
- Open/close is serialized on the UI thread at a frame boundary; no mid-frame layer mutation.
- A failed content instance shows a deterministic fallback and reports to the error boundary.

Layer stack invariants:

- Each layer is ordered by `(priority, open_sequence)`; tooltips/toasts never take focus.
- A modal entry makes all lower layers inert for pointer/keyboard/semantics.
- `OverlayHost` owns the layer nodes; content is not reparented into arbitrary scene nodes.
- Overlay nodes participate in dirty/style/layout/paint like normal nodes but are clipped to
  the host and rendered after the base layer.
- Closing removes semantics/pointer targets before animation completes.

Positioning:

```text
anchor_rect (window logical) + placement + size
  -> flip if outside main axis
  -> shift/clamp to safe area and IME inset
  -> track anchor layout/scroll/resize at frame boundary
```

- Anchor removal closes by default; a stable `uiId` re-anchor is opt-in.
- Multi-monitor positions use the window's display work area; never cross displays implicitly.
- Virtual keyboard/IME insets update placement before paint.

Back/dismiss ordering (canonical):

1. Topmost dismissible overlay (menu/popover/tooltip/dialog with `dismiss_on_back`).
2. Topmost modal route/sheet.
3. Navigation stack pop.
4. OS exit/close policy.

- An overlay that consumes back must not also bubble to navigation.
- Destructive dialogs default to no click-outside dismissal.
- Route change closes transient overlays; modal routes are navigation entries, not ad-hoc
  overlays.

Failure/edge cases:

- Host destroyed/window closed: close all entries, cancel async, no callbacks after teardown.
- Anchor layout changes during animation: retarget at frame boundary, not per event.
- Async content resolves after close: discard by entry epoch.
- Nested overlays: only topmost modal owns focus; closing restores the previous entry.
- Multiple hosts: overlays belong to one host; cross-window overlays require a window spec.

Remaining design gaps: scene/dirty/semantics integration/multiple hosts (G-08), back ordering
(G-09), cross-window/nested menus (Q-07/Q-08).

## 9. Open questions

- Multiple overlay hosts and cross-window overlays.
- Nested overlay stacks and focus ownership.
- Overlay transitions with navigation.
- Whether toasts/dialogs are framework primitives or library components.
- Virtualized overlay lists and large menus.
