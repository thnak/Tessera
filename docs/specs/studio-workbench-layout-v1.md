# Studio workbench v1: dock and layout model, constraints, library choice

Status: library choice (flexlayout-react 0.11.2, section 5.4) Accepted by the owner 2026-11-19; the
model and the rest are proposed until their slices land. Companion of [studio-workbench-v1.md](studio-workbench-v1.md) (commands, menus,
toolbar, slice plan, open decisions). Implements the design that
[#516](https://github.com/thnak/Tessera/issues/516) requires "before code", and supersedes the
"three panes inside one right-hand column" rule of [../design/studio-page.md](../design/studio-page.md)
section 1 once W5 lands. Related:
[ADR-054](../../decisions/ADR-054-studio-front-end-react-vite.md) decision 3 (React renders the canvas
once, the engine owns the surface), [ADR-060](../../decisions/ADR-060-studio-services-in-web-workers.md).

## 1. The layout model

The dock is a **tree of splits and tab groups over named panels**. The model is Tessera's own JSON
and pure operations (`workbench-core`); a dock library, if adopted, is an *adapter* that renders it
and reports user gestures back as operations. The library's own serialization is never persisted
(rationale: section 2.4).

### 1.1 Vocabulary

| Term | Meaning |
|---|---|
| Panel definition | static, in `panels.json`: what a panel is |
| Panel instance | a panel present in the layout; at most one per singleton definition (all V1 panels are singletons) |
| Group | an ordered tab strip of panel instances, one active; the unit of "where a panel is" |
| Split | a row or column of groups and nested splits with proportional sizes |
| Root | exactly one split (or one group) |
| Shell | the VS Code-shaped frame around the dock: title/menu bar, optional toolbar, **activity bar** (icon column choosing the side bar view), status bar. Visibility flags live in the layout document's `shell` block |
| Side bar | a group-like container (left by default, right allowed) holding view panels (`project` now; `search`, `source-control` reserved); one view visible at a time, switched by the activity bar |
| Editor area | the central splits holding the `editor` panel (and later editor groups, main spec section 6) |
| Panel area | the bottom group of the default layout (Problems/Output analogues: Diagnostics, Mock console, Call trace, Platform events); it can be collapsed, maximized, or dragged elsewhere |
| Region | the shell areas collectively: menu bar, toolbar, activity bar, dock, status bar; only the dock content is user-arrangeable |

### 1.2 Panel definitions (`panels.json`)

```json
{
  "id": "canvas",
  "title": "Canvas",
  "icon": "canvas",
  "singleton": true,
  "closable": true,
  "defaultLocation": { "group": "preview" },
  "minSize": { "w": 240, "h": 160 },
  "lazyModule": "tessera_canvas",
  "requires": ["webgpu"],
  "drawsWhenHidden": false,
  "openCommand": "view.togglePanel"
}
```

| Field | Rule |
|---|---|
| `id` | stable, kebab-or-lower id; layout documents refer to it forever (ids are append-only like command ids) |
| `lazyModule` | the Wasm module imported **when the panel is first visible** (section 4.2); absent for panels with no module |
| `requires` | capability keys (`webgpu`); an unmet `requires` renders the panel's named unsupported state in that panel only (section 4.3) |
| `drawsWhenHidden` | must be `false` for every V1 panel that paints (section 4.2) |
| `minSize` | CSS px; the dock never makes a visible group smaller; below it the group collapses to its tab strip |
| `openCommand` | always `view.togglePanel` with `{ "panelId": id }`; the command is how a closed panel returns |

V1 panels: `project` (tree), `editor`, `canvas`, `scene`, `diagnostics`, `platform-events`,
`mock-console`, `call-trace`. `platform-events` is owned by
[#448](https://github.com/thnak/Tessera/issues/448) and `mock-console`/`call-trace` by
[studio-mock-services-v1.md](studio-mock-services-v1.md); until their slices land they are `reserved`
(not offered, not in the default layout). Reserved for the IDE slices: `problems`, `source-control`.

The editor is one panel whose *content* is the open-file tab strip ([#431](https://github.com/thnak/Tessera/issues/431));
file tabs are not dock panels. Splitting the editor into two files side by side is out of scope for V1.

### 1.3 The layout document

```json
{
  "format": "tessera.studio.layout",
  "version": 1,
  "root": {
    "type": "split", "dir": "row", "sizes": [0.18, 0.82],
    "children": [
      { "type": "group", "id": "g-project", "panels": ["project"], "active": "project" },
      { "type": "split", "dir": "column", "sizes": [0.72, 0.28], "children": [
        { "type": "split", "dir": "row", "sizes": [0.5, 0.5], "children": [
          { "type": "group", "id": "g-editor", "panels": ["editor"], "active": "editor" },
          { "type": "group", "id": "g-preview", "panels": ["canvas", "scene"], "active": "canvas" }
        ] },
        { "type": "group", "id": "g-bottom",
          "panels": ["diagnostics", "mock-console", "call-trace", "platform-events"], "active": "diagnostics" }
      ] }
    ]
  },
  "maximized": null,
  "closed": [],
  "shell": { "activityBar": true, "statusBar": true, "toolbar": true, "sideBar": "left", "zen": false },
  "collapsed": ["g-bottom"]
}
```

Rules (all enforced by `validateLayout`, a hand-written validator with no schema library):

- `format` and `version` are required; `version` is a positive integer.
- `sizes` are fractions that sum to 1 within 1e-6; a split has at least two children; a split never
  has a split child of the same `dir` (the model normalizes by flattening; this is what keeps
  serialization canonical and diffs readable).
- Every panel id appears **at most once** across all groups and `closed`; a group has at least one
  panel (an emptied group is removed by the operation that emptied it); `active` names a panel in
  that group.
- `closed` lists panels the viewer closed, so a *new* panel shipped in a later version can be told
  apart from one the viewer chose to close (migration, section 2.3).
- `collapsed` lists group ids that are hidden but keep their size, panels and active tab (VS Code's
  hidden panel area). Toggling (`Ctrl+J`, a status-bar item, a new diagnostic arriving when
  `diagnostics.autoOpen` is on) removes the id; it does not move or remount panels. A group id in
  `collapsed` must exist. **The default layout lists `g-bottom` here.**
- `shell` holds the booleans of the shell (`zen` is transient: never written to storage).
- `maximized` is `null` or a panel id (one group fills the dock; the others stay mounted).
- Group ids are `g-<slug>` strings, unique, minted by the model (never by a library).
- Size limit 64 KiB on read; deeper than 8 levels is invalid.

### 1.4 Operations (pure functions over the model)

`openPanel(id, where?)`, `closePanel(id)`, `movePanel(id, target)`, `activatePanel(id)`,
`splitGroup(groupId, dir, panelId)`, `resizeSplit(path, sizes)`, `maximize(id | null)`,
`resetLayout()`, `collapseGroup(groupId, bool)`, `applyPreset(id)`. A move `target` is one of `{ group, index? }` (join as a tab), or
`{ edge: "left"|"right"|"top"|"bottom", of: groupId | "root" }` (new split). Each returns a new
layout plus a `change` describing which panel instances changed visibility; the renderer uses that
to call the panel host contract (section 4.2). The operations are the **only** way the layout
changes: drag-and-drop, menu items, palette commands and keyboard moves all end here, so they are
testable without a browser and a Tessera-built UI gets them for free.

Commands (registered in the workbench registry): `view.togglePanel {panelId}`, `view.togglePanelArea`,
`view.toggleSideBar`, `view.zenMode`, `view.layoutPreset {presetId}`,
`view.movePanel {panelId, target}`, `view.maximizePanel`, `view.resetLayout`,
`view.focusNextRegion`, `view.focusPanel {panelId}`.

## 2. Serialization, versioning, migration

### 2.1 Persistence

| Aspect | Rule |
|---|---|
| Where | `localStorage` key `tessera.studio.layout`, wrapped in try/catch for the empty/throwing case exactly as `lib/preferences.ts` does; the page renders the default layout when storage is unavailable |
| Scope | per viewer (per browser profile); never in a share link, never in project files ([#516](https://github.com/thnak/Tessera/issues/516)) |
| Writes | debounced 300 ms after the last operation and on `pagehide`; a quota error is swallowed (layout is a convenience) |
| Reads | once at boot, before first paint of the dock, so there is no flash of the default layout |
| Phone | the persisted layout is the *desktop* layout; the phone view is derived (section 3.4), so rotating a tablet or resizing a window never rewrites storage |
| Size | at most 64 KiB; larger is treated as corrupt |

### 2.2 Schema versioning

- `version` is bumped when the *meaning* of the document changes, not on every panel addition (a new
  panel is handled by section 2.3, not a version bump).
- The migration table is `migrations[n]: (layoutV_n) => layoutV_n+1`, pure and total, covered by a
  fixture per step (`tests/fixtures/studio-layout/v<n>.json`), the same pattern as the IR and share
  codec fixtures.
- A document with `version` greater than the code knows is **not** migrated down: it is kept under
  `tessera.studio.layout.backup`, the default layout is used for this session, and a notice appears in
  the status line ("Layout from a newer studio; using the default"). This is what lets a viewer open an
  older deployment without losing their layout.
- A document that fails validation after migration is handled the same way (backup, default, notice).
  A layout error is never a page error and never blocks boot.

### 2.3 Reconciliation (not a migration)

After migration and validation, `reconcile(layout, panelDefs)` makes the document match the running
build:

1. Panels whose definition no longer exists are dropped (and their groups if they empty).
2. Panels that are `reserved` are dropped from groups.
3. A panel that is **new** (defined, not in any group, not in `closed`) is placed at its
   `defaultLocation`, falling back to the group of the default layout that holds it, else a new tab in
   the last-created group. So shipping `problems` in a later release adds it to the bottom dock for
   everyone who has not closed it.
4. If `editor` is missing (never closable in the default; defined `closable: false`) it is restored.
5. Panels with unmet `requires` stay in the layout (their unsupported state is the panel's content).

### 2.4 Why not persist the library's JSON

Dock libraries serialize their internal tree (and some include DOM-facing options). Persisting that
couples storage to the library version and makes the V1 Tessera-built studio inherit or convert it.
Our document is small, canonical, validated and migrated by tests we own; the adapter maps it both
ways, and a round-trip test (model to library to model equals the original) is a W5 gate.

## 3. Default layout, reset and responsive behavior

### 3.1 Default layout

Owner direction 2026-11-19: VS Code and Visual Studio conventions, **default dock state collapsed**.
The default layout is the mock's arrangement (owner answer on #516) with the panel area hidden on
first load, like VS Code's first run:

```text
+--+----------+----------------------------+----------------------------+
|A | Project  | Editor                     | Canvas | Scene             |
|B | (tree)   | (file tabs)                | (active: Canvas)           |
+--+----------+----------------------------+----------------------------+
|  Panel area (collapsed): Diagnostics | Mock console | Call trace | Platform events
+------------------------------------------------------------------------+
```

Widths 18% / 41% / 41%; the panel area, when opened, takes 28% of the height and shows
`Diagnostics` first (the first-paint module). A collapsed panel area costs nothing: its panels are
not visible, so their lazy modules are not imported until it first opens. The status bar shows the
Diagnostics count as a clickable item (`view.togglePanelArea`), so a collapsed dock never hides an
error. Two earlier statements are superseded and flagged (main spec decisions 9 and 10): #516's
"Scene/Diagnostics as tabs" and the owner's answer that the bottom dock is open by default.

### 3.2 Reset

`view.resetLayout` is enabled when `layout.isDefault` is false. It replaces the in-memory layout with
`layout.default.json`, reconciles, writes storage, and moves focus to the editor. It does **not** touch
files, the active project, scroll or editor state, or panel *content* state. Panels that are
unchanged between the old and new layout stay mounted (no remount; the operation's `change` list is
what the renderer reads).

### 3.3 Open/close semantics

Close removes the panel instance from the layout (adds the id to `closed`) and **unmounts its
content** (Canvas releases its GPU device; section 4.2). Reopening re-creates it in its default
location or last location. Hiding (a non-active tab) does not unmount. "Close" and "hide" are
different states; the model has both.

### 3.3a Layout presets and zen mode

- A **preset** is a complete layout document in `layout.presets.json`: `default`, `editor-only`
  (preview in a collapsed side group), `preview-focus` (Canvas maximized beside a narrow editor),
  `side-by-side` (Editor | Canvas, panel area open). `view.layoutPreset` applies one through
  `applyPreset`, which goes through reconcile like a stored layout; the previous layout is kept for one
  undo step ("Undo layout change" in the notification). Saving user presets is deferred to W8.
- **Zen mode** (`Ctrl+K Z`) sets `shell.zen`: it hides menu bar, toolbar, activity bar, side bar,
  panel area and status bar without modifying `collapsed`; `Esc Esc` or the command exits. Zen is
  transient, so a reload never starts in zen. Panels stay mounted.

### 3.4 Phone-width collapse

Below `layout.phone` (viewport width under 720 CSS px, proposed) the dock renders **one visible group
at a time** from a flat list of all open panels:

- The persisted layout is not modified. The collapsed view is a pure function
  `collapse(layout) -> { panels: ordered, active }` ordered Editor, Canvas, Scene, Diagnostics, then the
  rest in layout reading order.
- A switcher (a `tablist`, scrollable) selects the visible panel; the others stay mounted and hidden
  (section 4.2), so no panel reloads on switch.
- Drag and split are unavailable; `view.movePanel` is disabled with `layout.phone`. Open/close still
  work.
- `document.documentElement.scrollWidth <= clientWidth` (no horizontal scroll) remains the gate.
- Resizing back above the breakpoint restores the desktop arrangement unchanged.

## 4. Hard constraints from #516

Each is a testable sentence and maps to a gate in section 4.5.

### 4.1 The canvas element and its WebGPU context are never re-parented or re-mounted

The engine owns the canvas surface and React renders the element once (ADR-054 decision 3). So:

- Panels are **kept mounted**, and moving a panel repositions it, never recreates it. Concretely the
  dock must use a panel retention mode in which a panel's DOM subtree is created once and survives
  group moves, tab switches, splits and resizes. Libraries differ: section 5 measures it.
- The Canvas panel host owns a stable `<div>`; the canvas element is created inside it exactly once
  per open of the panel; the host never receives a different `canvas` node.
- Pass criteria (measured in section 5.2 with a `MutationObserver` on the document and an identity
  check): across move-to-bottom, tab-into-another-group, hide, split-right the same `HTMLCanvasElement`
  object is present, with 0 removal records and 0 additional mounts, and `isContextLost()` stays false.
  Closing the panel is the only operation allowed to remove it.

### 4.2 Lazy modules, hidden panels never draw, panel host contract

```text
PanelHost (what workbench-core gives a panel, framework-agnostic)
  mount(container: HTMLElement): Disposable   // exactly once per open
  onVisibility(visible: boolean): void        // active tab && group visible && not phone-hidden && document visible
  onResize(widthCssPx, heightCssPx): void
  onClose(): void                             // after this, mount() may be called again by a later open
```

- **Lazy import.** `lazyModule` is `import()`ed on the first `onVisibility(true)`, not on `mount`
  (a panel in a background tab, or one restored hidden from storage, imports nothing). A closed Canvas
  has never acquired, or has released, its GPU device.
- **Hidden panels never draw.** On `onVisibility(false)` Canvas cancels its `requestAnimationFrame`
  loop and stops submitting; static UI therefore idles with zero presents, which is the engine's
  non-negotiable. The browser script counts frames while hidden (section 4.5).
- `visible` also goes false on `document.visibilityState === "hidden"`.
- Scene and Diagnostics follow the same contract; they have nothing to draw but must not do periodic
  work when hidden.

### 4.3 Failure isolation

- No WebGPU: `canvas.available` is false, Scene and Diagnostics work, and **Canvas alone** shows its
  named unsupported state (already implemented in `CanvasPane.tsx`; the dock keeps it).
- Each panel is wrapped in a boundary (React error boundary today, the same contract in Tessera). A
  throw in a panel render or in its host callbacks shows "This panel failed" with a **Reload panel**
  command and never unmounts siblings. A failed `lazyModule` import marks only that panel
  unavailable.
- A corrupt or newer layout never blocks boot (section 2.2).
- Service failures follow ADR-060: a dead worker disables the commands that list it in `services`, not
  the page.

### 4.4 Keyboard operability and accessibility

Required semantics (the browser gate asserts them; a library that cannot provide one must be wrapped):

| Element | Pattern |
|---|---|
| Menu bar | `role="menubar"`; top items `role="menuitem"` with `aria-haspopup="menu"` and `aria-expanded`; submenus `role="menu"`; `menuitemcheckbox`/`menuitemradio` with `aria-checked`; disabled items `aria-disabled="true"` and stay focusable. Keys: Left/Right between menus, Up/Down in a menu, Enter/Space activate, Esc closes and returns focus, Alt+letter mnemonic, Home/End, type-ahead |
| Toolbar | `role="toolbar"`, roving tabindex, one tab stop |
| Dock groups | each group's strip `role="tablist"` (`aria-orientation`), tabs `role="tab"` with `aria-selected`, `aria-controls`; the content `role="tabpanel"` with `aria-labelledby`; Left/Right/Home/End move between tabs; Delete or Ctrl+W-substitute closes the focused tab (a registry command) |
| Splitters | `role="separator"` with `aria-orientation`, `aria-valuenow/min/max` (percent) and `tabindex="0"`; arrow keys resize by 16 px, Shift+arrow by 64 px; Home/End to min/max |
| Palette | combobox + listbox, `aria-activedescendant` |
| Status messages | existing `role="status"` regions stay |

Focus order is DOM order: menu bar, toolbar, then groups in reading order (left to right, top to
bottom), status line; DOM order must equal visual order, so the model orders groups by reading order
when it renders. `F6`/`Shift+F6` (`view.focusNextRegion`) cycle those regions. **Keyboard panel
moves** are commands, not a mouse-only gesture: `view.movePanel` with targets Left, Right, Bottom and
"Into group >" (a submenu listing groups by their active panel's title), all reachable from the
palette and the panel-tab context menu (`ContextMenu` or `Shift+F10`). Drag and drop is an
additional path, never the only one. Focus returns to the moved panel's tab after a move; an
`aria-live="polite"` message states the result ("Canvas moved to the bottom").

Sashes and drop zones (VS Code behavior, as model operations): a sash drag calls `resizeSplit`;
it **snaps** to the default fraction and to the group's `minSize`, and dragging a group below
`minSize` collapses it (`collapseGroup`); double-click resets the neighbors to the default
fractions. During a panel drag the adapter shows **drop-zone overlays** (centre for tab join, four
edges for a split, plus the side bar and panel-area regions) and highlights the target before
release; if the chosen library cannot style overlays this way the adapter draws them. Panel
maximize/restore (`view.maximizePanel`, panel header button, double-click on the tab strip) calls
`maximize`.

### 4.5 Rewrite of `scripts/ci/preview_browser_layout.mjs`

The script is rewritten, not deleted ([#516](https://github.com/thnak/Tessera/issues/516)). Today it
asserts three side-by-side columns and that a preview view switch moves nothing else. The new
assertions, at 1440x900 and 390x844:

1. Default layout: Project, Editor and Canvas side by side in that order; the panel area collapsed (no
   Diagnostics box until `Ctrl+J`, then a bottom group spanning the width); the status bar below,
   with the Diagnostics count item. Boxes read from `data-panel` attributes (the old
   `.region-*` classes go away).
2. Tab switch inside a group (Canvas to Scene and back) changes no other panel's box (the old
   "no view switch changes the workspace shape" rule, restated per group).
3. Each of Canvas, Scene, Diagnostics opens and closes independently from `View > Panels`; closing
   Canvas leaves no GPU device taken (device counter exposed by the engine session in test mode).
4. Move Canvas to the bottom and Diagnostics to the right by keyboard commands **and** by drag; after
   each, the Canvas element is the same object, 0 removals, `frames` keeps increasing, no re-init
   (the measurement of section 4.1).
5. Reload: the same layout returns (compare serialized models); with `localStorage` throwing, the
   default layout renders without a console error.
6. A hidden Canvas (background tab) produces zero frames over 1 s.
7. At 390 px: one visible group, a switcher, no horizontal scroll, every panel reachable, and the
   persisted desktop layout unchanged after a resize round trip.
8. ARIA assertions of section 4.4 (roles, one tab stop per roving set, separator values).
9. `Reset Layout` restores the default and does not change the open file or its dirty state.

It stays developer-run for the same reason as the other browser scripts (needs a WebGPU-capable
browser); the pure parts (model operations, migrations, reconciliation, when-clauses, keybinding
resolution) run in Node gates in CI.

## 5. Library choice

Requirements: keep every panel mounted through moves (4.1), lazy and hidden semantics are ours (4.2,
implemented in the host contract, not the library), keyboard/ARIA (4.4), JSON round trip to our model,
React 19, permissive license, small. Candidates: dockview, flexlayout-react, rc-dock, hand-built.

### 5.1 What the docs and source say

Read from each project's published package and docs (versions as installed 2026-11-19).

| | dockview 8.9.1 | flexlayout-react 0.11.2 | rc-dock 4.1.0 | hand-built |
|---|---|---|---|---|
| License | MIT | MIT | Apache-2.0 | ours (MIT) |
| Runtime deps | `dockview-core` (zero third-party; React binding is the separate `dockview-react` package) | none (React peer) | `lodash`, `classnames`, `rc-new-window`, `@rc-component/{menu,tabs,dropdown}` | none |
| Panel retention | per-panel `renderer: 'always'` keeps the DOM alive and hidden; docs say panel instance is destroyed only on removal and that 'always' is required so an iframe is not reloaded on a group move. In this mode content lives in an overlay container positioned over the group | `tabEnableRenderOnDemand: false` renders every tab; React state is preserved across moves between tabsets per its docs | `cached: true` keeps a tab's content, but measured below it is re-parented | by construction: we choose |
| Layout JSON | `toJSON()/fromJSON()`, library-shaped | `model.toJson()`, library-shaped | `saveLayout()/loadLayout()` | our model directly |
| ARIA | tablist/tab/tabpanel with `aria-selected`, `aria-controls`, roving `tabindex`, tab-strip key handling in the shipped source | tab roles and arrow/Enter/Delete key map (configurable `keyMap`), splitter with `aria-orientation` and arrow-key resize, an `aria-label`ed tablist; keyboard float/popout | little visible in the shipped bundle | ours to build and test |
| Keyboard panel move | not built in (we provide commands) | not built in (we provide commands) | not built in | commands |
| Floating / pop-out | floating groups and pop-out windows | float and pop-out | float and pop-out window | out of scope |
| Phone collapse | none; we render `collapse(layout)` ourselves | none | none | ours |
| Maturity signals | active (published 2026-11), React/Vue/Angular bindings | active (2026-11), `caplin` GitHub organization | last publish 2026-10 | n/a |

Our collapse, lazy-visible and hidden-never-draws behavior is implemented in the host contract
(4.2), so no candidate has to provide it; what the library must provide is drag/split/resize
mechanics, retention and ARIA.

### 5.2 Measured: retention (canvas identity)

Throwaway Vite 7 + React 19 pages outside the repo, a panel holding a real `<canvas>` with a WebGL2
context, a `MutationObserver` counting removal of the canvas from the document, and moves issued
through each library's API (pointer DnD is not synthesized in this lane; see the caveat).

| Operation | dockview (`always`) | flexlayout-react (render on demand off) | rc-dock (`cached`) |
|---|---|---|---|
| Move Canvas to a new split below | same element, 0 removals | same element, 0 removals | same element, 1 removal |
| Tab Canvas into another group | same, 0 | same, 0 | same, 2 cumulative |
| Hide it (another tab active) | same, 0 | same, 0 | n/a |
| Split it right | same, 0 | same, 0 | same, 3 cumulative |
| Context lost at any step | never | never | never |
| Close the panel | unmounts, 1 removal (expected) | unmounts, 1 removal (expected) | unmounts, 4 cumulative |
| Layout JSON size, 3 panels | 580 B | 789 B | 393 B |

rc-dock re-parents the DOM node on every move; the identity and the context survive in Chromium, but
the requirement is "never re-parented", which rc-dock fails, so it is out. dockview and flexlayout both
satisfy 4.1 when configured as above (dockview only with `renderer: 'always'`; its default
`onlyWhenVisible` removes a hidden panel's element, which the docs state).

Caveat: a synthetic HTML5 drag of a dockview tab onto a group was rejected by Playwright because
`always` mode places an overlay above groups (`dv-render-overlay`) that intercepted the pointer
action. dockview handles real drags itself, but this lane did not prove one. A real pointer drag
against the chosen library is a W0/W5 acceptance item, not a measured fact here.

### 5.3 Measured: bundle cost

Method: the same Vite 7.3.8 production build (esbuild minify, default settings), React 19.3.1 and
ReactDOM identical in all four builds; entry = React mount plus the library's minimal layout of 2 to 3
panels, a layout save and a restore. Sizes are the sum of emitted `assets/*` files, gzip level 9 via
Python `zlib` (the encoder `tests/golden/perf/preview_budget.json` records for
`download_gzip_bytes`). Delta is relative to the React-only baseline of 69,305 B gzip.

| Build | JS gzip | CSS gzip | Total gzip | Delta vs baseline | JS raw | CSS raw |
|---|---:|---:|---:|---:|---:|---:|
| baseline (React only) | 69,305 | 0 | 69,305 | 0 | 222,487 | 0 |
| flexlayout-react 0.11.2 | 119,735 | 3,897 | 123,632 | **+54,327** | 420,087 | 24,204 |
| rc-dock 4.1.0 | 135,161 | 3,000 | 138,161 | +68,856 | 442,754 | 12,609 |
| dockview-react 8.9.1 | 169,231 | 10,768 | 179,999 | **+110,694** | 649,702 | 142,625 |

Against the budget file: `download_gzip_bytes` records the page at **2,771,689 B gzip** (React page
chunk 268,505 + CSS 4,506 + three Wasm modules and fonts; the enforced `budget` pin is 10,000,000,
the two-sided ratchet of its `tolerance`). So:

- flexlayout-react adds **+54,327 B = +1.96 %** of the recorded total (+19.9 % of the page JS+CSS
  chunk of 273,011 B);
- dockview adds **+110,694 B = +3.99 %** of the total (+40.5 % of the page chunk).

The enforced pin is 10,000,000 B with `worse: 5.0 %` and `better: 80.0 %`, so either library stays
inside the band (the lower bound is 2,000,000 B, still below the recorded total); the adopting slice
nonetheless re-records the measured total as earlier slices did
([studio-page.md](../design/studio-page.md) section 6). dockview's CSS is 142 kB raw because it ships all of its themes; a custom
theme CSS would cut it, which was not measured. The hand-built estimate (not measured, labelled
as such) is 8 to 15 kB gzip for splits, tab strips, drag and keyboard, at the price of ownership
of drag-and-drop, hit-testing, touch and ARIA behavior.

### 5.4 Recommendation (Accepted 2026-11-19)

The owner accepted this choice: flexlayout-react **0.11.2, exact-pinned**; dockview is the fallback only
if flexlayout fails a W0 check. W0 results are recorded in section 5.6.

**flexlayout-react, behind our own layout model, with the adapter kept thin enough to replace.**

Evidence: it meets 4.1 in the measurement (0 removals through all moves), has the smallest cost
(+54,327 B gzip, half of dockview's), zero runtime dependencies, MIT, a serializable model whose JSON
we translate from our own, and the most keyboard/ARIA provision of the three in its shipped source
(arrow and Enter/Delete tab handling, an overridable `keyMap`, arrow-key splitter resize with
`aria-orientation`). Its model operations (`Actions.moveNode`, `selectTab`, `deleteTab`) map one-to-one to
section 1.4, which keeps the adapter small.

Risks to retire in W0 before committing to it:

- **Maintenance and bus factor are not verified here.** Mitigated by the adapter boundary: the exit is the hand-built dock,
  and our model, operations, migrations, and tests are library independent.
- **Not proven here:** a real pointer drag onto each drop target, the exact ARIA output of its
  splitter and tabs against section 4.4 (read from source, not run against a screen reader), and
  behavior with the real Canvas (WebGPU) rather than a WebGL2 stand-in.
- **dockview is the alternative** if W0 finds a gap: it also satisfies 4.1 (with `always`), has
  richer drag affordances and floating groups, and costs +110,694 B gzip (+3.99 %). Choose it only
  if flexlayout fails a W0 check, because it doubles the bundle cost.
- **Not recommended: rc-dock** (re-parents, Apache-2.0 with six dependencies including `lodash`).

### 5.5 Dependency documentation required

If flexlayout-react is adopted (W0 lands these with the dependency, per
[dependencies.md](dependencies.md) and AGENTS.md):

1. `docs/specs/dependencies.md` section 1 row, next to CodeMirror and fflate:
   `flexlayout-react | V1 design-time (studio) | MIT | designer/web | Dock layout for the hosted studio; bundled and version-pinned; kept behind the workbench layout adapter so the V1 Tessera-built studio does not depend on it; cost about +54 kB gzip recorded in tests/golden/perf/preview_budget.json`.
2. `THIRD_PARTY_NOTICES.md`: a row `flexlayout-react | MIT | the copyright holder named in the package `LICENSE`; bundled in the hosted studio page`
   (copy the holder verbatim from the package's `LICENSE` at adoption).
3. `designer/web/package.json` pin with an exact version (no caret), as CodeMirror is pinned.
4. Re-record `download_gzip_bytes` in `tests/golden/perf/preview_budget.json` with the measured value
   and add a "section 6.x" entry in `docs/design/studio-page.md`.
5. If dockview were chosen instead: `dockview-react`, `dockview`, `dockview-core` (all MIT) are three
   packages and three notice entries.
6. `workbench-core` has no dependency entries; it is first-party.

### 5.6 W0 results (flexlayout-react 0.11.2, real Chromium, real WebGPU)

Measured 2026-11-19 by `scripts/ci/workbench_dock_spike.mjs` against the throwaway page
`designer/web/spike/` (deleted when W5 lands): headed Chromium, a real `GPUCanvasContext` that clears
every frame, **real pointer drags** (mouse down, 30 or so moves, mouse up; no synthetic `DragEvent`),
and keyboard-only docking. The drag was **not blocked by an overlay**: flexlayout renders tab content in
one flat layer and its own drag overlay, so the "always" overlay problem seen with dockview in section
5.2 does not occur. Result: 37 pass, 3 gaps, 0 failures. dockview is therefore **not** needed.

| Check | Result |
|---|---|
| Real pointer drag, tab joins another group | pass; model parent changes, same `HTMLCanvasElement`, 0 removals, 1 mount, 1 GPU context |
| Real pointer drag onto an edge (new split) | pass; same checks |
| Editor `<textarea>` node and typed text across moves | pass (DOM node and value survive) |
| Hidden Canvas draws nothing | pass; 0 frames over 1 s while Scene is the active tab, drawing resumes on show |
| Lazy module | pass; a Canvas restored hidden neither imports nor fetches its module until first visible (`visibility` node event) |
| Keyboard-only docking | pass through an adapter command (`Alt+Shift+Arrow`), focus returns to the moved tab, `aria-live` states the result; flexlayout has no keyboard move, as section 5.1 said |
| Failure isolation | pass; an error boundary per panel shows "This panel failed" and leaves Canvas and siblings untouched |
| Collapsed bottom group | usable as a flexlayout **border** (strip visible, content hidden, opens without touching Canvas). It keeps a visible strip, which differs from VS Code's fully hidden panel area; W5 either keeps the border or drops the group from the library model while collapsed (panels there have never been visible, so no mount is lost). Open W5 design point |
| JSON round trip, 390 px no horizontal scroll | pass (the phone collapse stays ours, section 3.4) |
| ARIA | tablist with `aria-orientation`, tab `aria-selected`/`aria-controls`, tabpanel `aria-labelledby`, one tab stop per strip, separators with `aria-orientation`, `aria-valuenow`, `tabindex=0`: all pass |

Gaps the adapter closes (all small, none blocks adoption): separators report constant
`aria-valuemin=0` and `aria-valuemax=100` instead of per-splitter limits; splitter arrow keys resize
by 10 px with no Shift step and no Home/End (section 4.4 wants 16/64 px and Home/End, implemented as a
capture-phase key handler calling `Actions.adjustWeights`); tab Left/Right and `Alt+W` close work as
shipped. Visual styling (VS Code colors, sizes, icons) is a W5 theme over the library CSS and is not
part of W0.

Not run: the engine's own Wasm canvas (no built modules in this lane; the stand-in is the same
`getContext("webgpu")` surface) and the Linux box (thnak-remote was unreachable). The enforced
`download_gzip_bytes` is **not** re-recorded in W0 because nothing from the library ships in the
studio build yet; the adopting slice (W5) re-records it with `scripts/check_preview_budget.py --update`.

## 6. What this does not do

- It does not implement anything; W0 to W9 in [studio-workbench-v1.md](studio-workbench-v1.md) do.
- It does not change the engine, the Wasm modules, `tessera.json`, or the share-link format. Layout is
  per-viewer and never in a link.
- It does not pick pop-out windows (a pop-out re-parents the canvas into another document); see open
  decision 8 of the main spec.
- It does not specify the palette's ranking, fuzzy matching or recent-commands list; slice W3 does.
