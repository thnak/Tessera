<!-- SPDX-License-Identifier: MIT -->

# Studio workbench dock: how it is built

Status: implemented by W4 ([#556](https://github.com/thnak/Tessera/issues/556)) and W5
([#560](https://github.com/thnak/Tessera/issues/560), implements
[#516](https://github.com/thnak/Tessera/issues/516)). The rules are normative in
[`studio-workbench-layout-v1.md`](../specs/studio-workbench-layout-v1.md) and
[ADR-062](../../decisions/ADR-062-studio-dock-library-flexlayout-react.md); this note records the
shape of the code and the reasons for it, so the next slice (W6, #563) does not rediscover them.

## 1. Two layers

```text
designer/web/src/workbench/   pure model: no React, no DOM, no library
designer/web/src/dock/        adapter: flexlayout-react behind codec / actions / hosts
```

The **model** (`workbench/`) owns Tessera's layout JSON (`tessera.studio.layout` v1: a split/group
tree plus `closed`, `collapsed`, `maximized`, `shell`). It has a hand-written validator, a canonical
flattening (`normalize`), operations that return `{layout, change}`, migration, reconcile against the
panel registry, the phone projection, and persistence. It is exercised by
`scripts/ci/workbench_layout.mjs` (35 tests, a versioned fixture under
`tests/fixtures/studio-layout/`). Layout operations are plain functions (`createLayoutStore`,
`createCommands`), so the command registry (W1) wires `view.*` and `layout.reset` without touching React.

The **adapter** (`dock/`) is the only code that imports `flexlayout-react`. It is *controlled*: the
library never owns the layout.

| File | Job |
|---|---|
| `codec.ts` | layout JSON to the library's model JSON, and the reverse |
| `actions.ts` | intercepts every library action in `onAction`, maps it to a model operation (`gestureOf`) and cancels it |
| `hosts.ts`, `PanelSlot.tsx` | one persistent host element per open panel, adopted by the slot the library lays out |
| `separators.ts` | real `aria-valuemin/valuemax/valuenow` on separators, splitter key arithmetic |
| `FlexDock.tsx`, `WorkbenchDock.tsx` | the React surface: ARIA patching, Alt+Shift+Arrow moves, double-click maximize |
| `use-workbench.ts` | store subscription, persistence, the `Ctrl+J` stand-in for the registry |
| `PanelBoundary.tsx` | an error boundary per panel (failure isolation) |

`scripts/ci/workbench_dock_core.mjs` (12 tests) round-trips the model through the real library `Model`
in Node. `scripts/ci/workbench_dock.mjs` is the developer-run real-browser acceptance (real mouse
drags, keyboard-only docking, reload persistence, throwing storage, 390 px).

## 2. Why the canvas never moves

Panel content is **not** rendered inside the library's tab content. Each open panel has a host
`<div data-panel=id>` created once; React portals render the panel into it, and `PanelSlot` appends
that host to the slot the library currently lays out. A move therefore moves a host between slots
with `appendChild`, never a React remount, and the `<canvas>` inside is the same node throughout
(the gate asserts the same element and 0 removals across real drags).

Two details were needed to make that true:

- The library `Model` is rebuilt on every layout change with `Model.fromJson(json, previous)` held in a
  ref, so the library adopts the existing moveable elements instead of replacing them. Without
  `previous`, every swap removed the canvas twice. This is an internal-feeling API; ADR-062 records the
  dependency, and `workbench_dock_core.mjs` fails if an upgrade changes it.
- Closing a panel unmounts it and disposes its session (`useSessions(..., present)`; the canvas
  `dispose()` destroys the `GPUDevice`, observable as `globalThis.__tesseraGpu.live`). A panel that is
  merely hidden or collapsed stays mounted in a detached host, so Diagnostics keeps writing the
  status-bar count while the panel area is collapsed.

## 3. The visible projection

A collapsed group is not given to the library at all (spec section 3.3). Dropping it can fold two
splits into one, so the library's row ids and sizes no longer match the real tree. Row ids are
`row:<path>` in the *visible* tree, and a sash drag or splitter key is lifted back onto the real tree
by `resizeVisibleSplit` (the `resize` operation with `visible: true`). Tabset ids are group ids and
tab ids are panel ids, so a library gesture maps to a model operation without a lookup table.

## 4. Phone width

At 720 px the adapter shows `phoneLayout()`: one group, `g-phone`, in the order editor, canvas, scene,
diagnostics, rest. The library's own tab strip is the switcher; drag and maximize are disabled. The
stored layout is not touched, so crossing the breakpoint back restores the desktop arrangement.
Crossing it, or collapsing a group that holds the canvas, detaches hosts; that re-parents the canvas
element. The owner accepted this (2026-11-19): the spec allows it for an explicit breakpoint change, it
is the one place the no-reparent rule is relaxed, and it is recorded here.

## 5. Panel runtime

`createPanelRuntime` implements the host contract (`mount`, `onVisibility`, `onResize`, `onClose`).
`usePanelVisibility` (`page-panels.ts`) starts a session when its panel first becomes visible and tells
it when it is hidden; Diagnostics always starts. A hidden panel must not draw; that guarantee is the
session's, driven by `onVisibility`.

## 6. Known gaps (decisions for the owner)

- Sash snap to the default fraction, double-click reset and collapse-below-`minSize` (spec 4.4) are not
  implemented, by owner decision (2026-11-19): a follow-up, not part of the command wiring. The library
  offers no hook and the adapter does not draw sashes itself.
- Tab count badges were lost with the old tab strip. The tab context menu (`panel.tab`, Shift+F10) is back:
  Maximize or Restore, Move Panel (Left, Right, Top, Bottom, Into Group), Close Panel
  (`page-layout-commands.ts`, section 7).
- Not run: the real-WebGPU checks (a hidden Canvas draws zero frames, the frames keep painting after a
  move), because no Wasm build exists on the development machine. The no-WebGPU check (Scene and
  Diagnostics keep working with `navigator.gpu` absent) is in `workbench_dock.mjs`.
- `scripts/ci/preview_browser_layout.mjs` was edited minimally for the dock DOM; its
  project-dependent checks were already stale after #391 (the workspace starts empty). W6 owns the
  rewrite.

## 7. The commands over the dock

The dock-owned command ids are live and each handler is one call into the layout store
(`page-layout-commands.ts`, `workbench/store.ts`), so a menu item, a chord, a palette entry and a tab
context menu run the same code: `view.toggleSideBar` (the Project group), `view.togglePanelArea`,
`view.togglePanel`, `view.movePanel`, `view.movePanelToGroup`, `view.maximizePanel`, `view.layoutPreset`,
`view.resetLayout`, `view.zenMode` and `view.exitZenMode`, `view.toggleStatusBar`,
`view.focusNextRegion` and `view.focusPreviousRegion`, `view.undoLayoutChange`. The menu check marks read
the layout through context keys (`layout.sideBar`, `layout.panelArea`, `layout.statusBar`, `layout.zen`,
`layout.preset`, `panel.<id>.visible`). Presets and reset keep one undo step, offered as an "Undo layout
change" notification action; it is valid exactly while the layout is still the object the preset produced.
Zen mode is `shell.zen`: the dock shows the editor alone as a view of the layout (`maximized` and
`collapsed` untouched), the toolbar and status line are not drawn, and panels stay mounted but report
hidden. `view.toggleActivityBar` stays reserved (there is no activity bar). On a phone, toggles reveal or
leave a panel instead of collapsing a group.

Entering zen mode raises one notification, "Zen mode: press Esc twice or F1 and choose Exit Zen Mode to leave", with an
Exit Zen Mode action, because the menu bar that could say so is hidden. It is raised once per page session (re-entering
does not repeat it) and goes through the notification service, so do-not-disturb suppresses the toast but the hint stays
in the bell history.

## 8. `?view=` and the stored layout

`?view=<panel>` is deliberate (owner decision, 2026-11-19): at boot the named panel is opened or revealed even when the
stored layout had closed it, and a later change of the shown view writes `?view=` back, so a reload keeps showing it.
`workbench_commands_dock.mjs` covers it: close Canvas, reload with `?view=canvas`, and the panel is back.

