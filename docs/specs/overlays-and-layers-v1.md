# Overlays and layers v1

Status: proposed (Big Steps B2 and C, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big
Steps"). Decision:
[ADR-029](../../decisions/ADR-029-interaction-completeness.md) (interaction completeness: text
editing, scrolling, overlays, forms). Companion to
[`component-model-v1.md`](component-model-v1.md), [`input.md`](../design/input.md),
[`accessibility-v1.md`](accessibility-v1.md), and [`navigation-v1.md`](navigation-v1.md).

Real apps need content above the base tree: dialogs, menus, popovers, tooltips, toasts. Portals
are rejected in V1 (V1 = first-release scope, [AGENTS.md](../../AGENTS.md#non-negotiables) rule 8; see
[`component-model-v1.md`](component-model-v1.md)); this spec defines an explicit overlay/layer system instead.

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
- Clamp to window safe area (see [`responsive.md`](../design/responsive.md)), the display work
  area, and IME (input method editor) inset; never cover the caret/IME.
- Scroll tracking updates position at frame boundaries; no layout in the pointer handler.
- Virtual keyboard/insets change placement; recompute and clamp.
- Anchor removed -> overlay closes or re-anchors; default close (re-anchoring is opt-in, see
  section 8.1).

## 4. Modal and focus

- Modal: background inert (pointer/keyboard/accessibility), focus trap, restore previous focus.
- Escape/back dismiss policy is explicit; destructive confirmation is separate.
- Click-outside dismissal is opt-in; menus/popovers default on, dialogs default off.
- Stack: topmost modal owns focus; closing reveals the previous modal.
- Route change/window blur/background closes transient overlays (the closing policy is not
  specified further); modals may persist.
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
- Overlay close/removal announces its closure (which overlays announce is not specified; see
  [`accessibility-v1.md`](accessibility-v1.md)); no focus loss to nowhere.

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

Gap-closure decisions (`G-` IDs refer to
[`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md)):

- **G-08**: one default `OverlayHost` per window; additional hosts are explicit. Layer nodes are
  children of the host (no reparenting), render after base, clip to host bounds, and modal
  entries make lower layers inert in input and semantics. Cross-window overlays require a
  window spec and are not in V1.
- **G-09**: a shared `BackDispatcher` (also described in
  [`navigation-v1.md`](navigation-v1.md) section 10.1) orders handlers: topmost dismissible overlay -> modal
  route -> route stack -> OS exit. A handler returning handled stops propagation; overlay and
  navigation cannot both act on one back event.

## 9. Deferred/open questions

Items are open (no decision yet) unless stated otherwise.

- Cross-window overlays, multiple hosts, and nested-host ownership.
- Sheet drag-to-dismiss and nested-menu interaction details.
- Toast queue/priority policy (library; no decision yet).
- Overlay transition choreography with navigation (visual only).
- Whether toasts/dialogs are framework primitives or library components.
- Virtualized overlay lists and large menus.
- The `tessera-ui` surface (`Dialog`, `Popover` and friends): the compiler cannot yet open an
  overlay from TSX.
- Initial focus and focus restore for non-modal overlays (the host has no `FocusManager` access).
- Fallback content and the error-boundary report for a failed mount.

## 10. Implementation (issue #302)

Slice 1 is the layer core (section 10.1); slice 2 is placement, dismissal and the input hook
(section 10.2). The `tessera-ui` library surface and non-modal initial focus are deferred (see
section 9).

### 10.1 Layer core

- `include/tessera/ui/overlay/overlay.hpp`, `src/ui/scene/overlay_host.cpp`: `OverlayHost`,
  `OverlaySpec`, `OverlayHandle`. `AppHost::overlays()` creates the window's default host on first
  use, so a scene that never opens an overlay gains no nodes; `AppHost::open_overlay` and
  `close_overlay` also wake an idle loop.
- Layers are four absolute, window-filling nodes under the scene root (overlay, modal, tooltip,
  toast; base is the app content), stacked by explicit z-index 1000 to 4000. Each is
  `pointer-events: none`, so an empty layer never shadows the base; the entry content opts back in.
- `open`, `close` and `close_if` only record. `flush()` runs at the start of `begin_frame`, closes
  first, then mounts, and repeats up to four rounds for opens made from a mount callback. Handles
  are never reused, so a stale handle is a no-op. An entry closed before its first flush never
  mounts.
- Entries are ordered by `(priority, open order)` inside a layer. A mount callback runs before
  anything is attached; a throw is caught, counted (`failed_count()`) and leaves the scene
  untouched. The fallback content and error-boundary report of section 8.1 are not wired yet.
- A modal entry adds a full-window scrim (it takes every pointer hit the content does not, so the
  base and lower layers are inert for the pointer) and a content root with `focus_trap`, role
  `dialog` and `aria-modal`. Focus moves in and is restored through the existing focus-scope code.
  Tooltip (role `tooltip`) and toast (role `alert`) never take focus.
- Semantics: a visible `aria-modal="true"` node makes every node that precedes it in tree order,
  except its ancestors, inert in the accessibility tree (`BuildStats::skipped_inert`). Layers after
  the modal, so toasts and tooltips, stay exposed.
- Overlay nodes take ids from `OverlayHost::kFirstNodeId` (`1 << 62`).

Tests: ctest `overlay_host` (`tests/unit/overlay_host_tests.cpp`).

### 10.2 Placement, dismissal and input

- **Placement** (`include/tessera/ui/overlay/placement.hpp`, `src/ui/scene/overlay_placement.cpp`):
  `compute_placement` is a pure function. The anchor is a window, a rectangle or a node id. For a
  rectangle or node the overlay sits outside the chosen side (`Top`, `Bottom`, `Start`, `End`,
  `Center`; start and end swap under RTL), `gap` away, aligned at the anchor's start, centre or end.
  It flips to the opposite side when the chosen one overflows and the other fits or has more room,
  then shifts along both axes to stay inside the viewport; a box larger than the viewport sits at
  its start edge. For a window anchor the side is the window edge the overlay sits against, inside
  the safe viewport (a top-centre toast is `Top` with a window anchor). Non-finite input counts as
  zero.
- **Host** (`src/ui/scene/overlay_place.cpp`): `OverlayHost::place` runs once per frame, after
  layout, because it needs the content size and the anchor bounds. It resolves the anchor each
  frame (a node is looked up by id, so scroll and layout changes are followed), computes the
  placement, and writes `left` and `top` on the absolutely positioned content only when the content
  moved, so a settled overlay causes no layout. `AppHost` passes the viewport: the window minus
  system bars, cutout, keyboard, gesture zones and hinge (`SafeAreaState`), and relayouts once when
  something moved. An anchor node that is gone closes the entry; re-anchoring by stable id is not
  built.
- **Dismissal** (`src/ui/scene/overlay_dismiss.cpp`): per entry `dismiss_on_escape`,
  `dismiss_on_outside`, `persistent` and `on_dismiss(handle, reason)`. Unset flags take the layer
  default: Escape and back dismiss everything but a toast; a press outside dismisses overlay-layer
  entries only (dialogs need an explicit choice, tooltips and toasts are transient on their own).
  `handle_escape` and `handle_back` dismiss the topmost entry that opted in, top layer first; a modal
  that did not opt in stops the search. This is the first handler of the back order of section
  8.1; the modal route, route stack and OS exit follow it once navigation exists.
  `handle_pointer_down` closes the entries that opted in and do not contain the press, top first,
  down to the first dialog (a dialog is inert beneath it, whatever the press hit); a press on a
  tooltip or toast, inside an entry's content, or on the entry's anchor node dismisses nothing, so
  a trigger can toggle its own popover. `close_transient` (window blur) closes the non-persistent
  overlay and tooltip entries. `on_dismiss` is not called for `close`, `close_if` or an anchor that
  disappeared.
- **Input hook**: `InputPump::set_input_filter` sees every event before the router and the gesture
  arena; returning true consumes it. `AppHost::handle_overlay_input` is the filter the window
  installs: an Escape press the overlays take is consumed together with its release; a press outside
  and a window blur never are, so the press still reaches the scene.
- **Live regions**: toast content carries `aria-live="polite"` next to role `alert`. The semantics
  tree builder does not interpret `aria-live` yet, so announcements wait on that work.

Tests: ctest `overlay_placement`, `overlay_dismiss`.
