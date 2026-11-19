# ADR-062: The studio dock uses flexlayout-react behind our own layout model

- Status: Accepted (2026-11-19, owner review of the workbench spec; W0 evidence recorded)
- Date: 2026-11-19
- Related: [ADR-054](ADR-054-studio-front-end-react-vite.md) (React renders the canvas once),
  [ADR-060](ADR-060-studio-services-in-web-workers.md) (V1 studio is rebuilt with Tessera),
  [`studio-workbench-layout-v1.md`](../docs/specs/studio-workbench-layout-v1.md) (sections 4, 5),
  [`studio-workbench-v1.md`](../docs/specs/studio-workbench-v1.md) (slice plan, decisions 1 and 2),
  [#516](https://github.com/thnak/Tessera/issues/516), [`dependencies.md`](../docs/specs/dependencies.md)

## Context

The studio needs a dock: drag, split, tab, resize, maximize, restore on reload. Issue #516 adds hard
constraints: the canvas element and its WebGPU context are never re-parented or re-mounted by a move,
a hidden panel never draws, Wasm modules load per panel on first visibility, a failing panel never
takes a sibling down, and everything is keyboard operable. The V1 studio is rebuilt with Tessera
(ADR-060), so nothing the dock owns may be a React or library artifact.

## Decision

1. **Adopt flexlayout-react 0.11.2** (MIT, no runtime dependencies, React 19 peer), exact-pinned in
   `designer/web/package.json`. dockview is the fallback only if flexlayout fails a W0 check; it did not.
2. **Our JSON is the canonical layout format.** The library's own serialization is never persisted. A thin
   adapter maps our model to the library and reports gestures back as our operations; the exit is a
   hand-built dock with the model, operations, migrations and tests unchanged.
3. **No floating windows or pop-outs in V1** (`supportsPopout={false}`): a pop-out would re-parent the canvas.
4. Panel retention is configured with `tabEnableRenderOnDemand: false`; lazy import, hidden-never-draws and
   failure isolation are implemented by our panel host contract and per-panel boundaries, not by the library.

## Evidence (W0)

Layout spec section 5.6: real Chromium with a real `GPUCanvasContext`, real pointer drags (not
synthetic DragEvents), keyboard-only docking. Same `HTMLCanvasElement`, 0 removals, 1 mount, 1 GPU
context through a tab join, an edge split, hide/show and keyboard moves; 0 frames while hidden; lazy
module neither fetched nor imported until first visible; error boundary isolates a throwing panel;
ARIA tablist/tab/tabpanel/separator roles present. Measured cost +54,327 B gzip (+1.96 % of the
recorded page total, layout spec 5.3).

## Consequences

- One new studio dependency, bundled and version-pinned; rows in `dependencies.md` and
  `THIRD_PARTY_NOTICES.md` (copyright holder Caplin Systems Ltd, from the package `LICENSE`).
- The adapter owns three small gaps: separator `aria-valuemin/max` per splitter, 16/64 px and Home/End
  splitter keys, and keyboard panel moves (commands). None needs a library patch.
- A bus-factor risk remains (the library is maintained by one company's open-source organization); it
  is bounded by decision 2.
- The enforced preview budget is re-recorded by the slice that ships the library in the studio build (W5).

- The adapter depends on one library behavior: `Model.fromJson(json, previous)` hands the previous model's
  tab nodes to the new one, so each tab keeps its content element and a move, a tab switch or a reset does
  not re-create or re-parent a panel's DOM (the canvas above all). A library upgrade must keep that behavior;
  `scripts/ci/workbench_dock_core.mjs` guards it (it rebuilds a model from the previous one and asserts the
  tab nodes are reused), and `workbench_dock.mjs` proves it in a real browser (same `HTMLCanvasElement`, no
  removals across moves).

## Alternatives

- **dockview** 8.9.1: also keeps panels alive (`renderer: 'always'`) but costs +110,694 B gzip and its
  always-mounted overlay complicates pointer drags. Kept as the fallback.
- **rc-dock** 4.1.0: re-parents the panel DOM node on every move (measured), Apache-2.0, six dependencies.
- **Hand-built**: no dependency, but we own drag hit-testing, touch and ARIA; kept as the adapter exit.

## Follow-up

W1/W4 (registry and pure layout model), W5 (adapter and dock renderer, deletes `designer/web/spike/`),
W6 (`preview_browser_layout.mjs` rewrite).
