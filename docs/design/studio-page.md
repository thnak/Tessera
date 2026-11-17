# The hosted studio page (issues #392 and #422)

Status: landed (Big Step D, V1-M4). Issues: [#392](https://github.com/thnak/Tessera/issues/392) (the three-
page merge) and [#422](https://github.com/thnak/Tessera/issues/422) (the React 19 + Vite conversion).
Decision: [ADR-054](../../decisions/ADR-054-studio-front-end-react-vite.md) (the conversion),
[ADR-045](../../decisions/ADR-045-browser-hosted-studio.md) (the browser-hosted studio, whose page-stack
wording ADR-054 amends and whose decisions stand).
Spec: [`designer-studio-v1.md`](../specs/designer-studio-v1.md) sections 2, 3, 7.1 and 10.
Predecessors: [`compiler-wasm.md`](compiler-wasm.md) sections 5-10 (P1 diagnostics, P2/P3 scene and canvas,
P4 input, P5 share links, P6 budgets) and [`reference-equantic-ui.md`](reference-equantic-ui.md) section 7.4.

The preview used to be three pages. #392 merged them into one `index.html`; #422 converted that page's
chrome from hand-written vanilla ES modules to **React 19 + TypeScript, built by Vite**. This note records
what the page is made of, how each preview is reached, how a pre-merge URL still works, where the boundary
between React and the engine sits, and why the P6 download budget was re-recorded twice.

## 1. One page, three views, one set of controls

The page has exactly one of each shared control -- a header and subtitle, a toolbar (viewport, DPR, theme),
a sample gallery, a "Copy share link" button, a source editor, a Run button and a Load-sample button -- and
three **views** that swap inside one pane:

| View | What it shows | Its Wasm module | When it loads |
|---|---|---|---|
| Diagnostics | the diagnostics list with source spans | `tessera_diagnostics` | at first paint (the default view) |
| Scene | the scene snapshot plus the `<automationId>` click script and its state cells | `tessera_scene_preview` | when the Scene tab is first opened |
| Canvas | the evaluated scene painted through WebGPU, with the frame counters | `tessera_canvas` | when the Canvas tab is first opened (device on the first Run there) |

The three Wasm builds are **not** merged, by either issue. Keeping them separate is also what keeps the
failure modes separate: a browser without WebGPU keeps Diagnostics and Scene and sees a named unsupported
state in Canvas only, and a module that fails to import leaves its own pane unavailable rather than the page
blank ([`reference-equantic-ui.md`](reference-equantic-ui.md) section 7.5).

**Laziness is the constraint both issues had to satisfy.** Diagnostics is the default view and the only
module on the first-paint path. A view's Wasm module is imported by a dynamic `import()` when its tab is
first shown, and the canvas view additionally defers its `requestAdapter`/`requestDevice` to the first Run on
that view -- a visitor who never opens Canvas never downloads `tessera_canvas.wasm` and never takes a GPU.
The React conversion did not change any of that: the view *sessions* (section 2) are the only code that calls
`import()`, and `scripts/ci/preview_page_checks.mjs` asserts that each of the three module URLs appears
exactly once in the built bundle, inside a dynamic import.

## 2. The stack, and the React/engine boundary

`designer/web` is a **Vite + React 19 + TypeScript** project (ADR-054 decisions 1-4):

- `package.json` / `package-lock.json` (committed) / `vite.config.ts` / `vite.ssr.config.ts` /
  `tsconfig.json`; `dist/` and `dist-ssr/` are git-ignored and never committed.
- `npm run build` is `tsc --noEmit` + `vite build` (the site) + `vite build --config vite.ssr.config.ts`
  (the gate-only bundle). The typecheck is part of the build: a type error is a build failure, not a runtime
  surprise in a browser no CI lane can drive.
- Styles are one static stylesheet (`src/styles/app.css`), imported by `src/main.tsx` so Vite emits a hashed
  `assets/index-<hash>.css`. No CSS-in-JS runtime, no second framework.
- The built `index.html` carries one `<script type="module" src="...">` and one `<link rel="stylesheet">`,
  both same-origin and content-hashed: **no inline script, no remote script**, so the strict CSP holds
  (ADR-045 decisions 1-3, ADR-054 decision 4).
- React is bundled into the static page and is never linked into an app binary or the Wasm engine
  (ADR-024, AGENTS.md non-negotiable 7). Node/npm is design-time tooling only.

### 2.1 What React renders, and what it does not

**React renders the studio chrome only** (ADR-054 decision 3). The engine still draws exactly one thing --
the preview canvas -- and the page remains its host.

| Region (`designer-studio-v1.md` section 3) | Component |
|---|---|
| studio chrome (header, view tabs, toolbar, gallery, share) | `regions/StudioChrome.tsx`, `regions/GalleryRegion.tsx` |
| workspace pane (the shared editor and the three view panes) | `regions/ViewPanes.tsx` |
| preview well (the canvas pane and its four states) | `regions/CanvasPane.tsx` |
| status line | `regions/StatusLine.tsx` |

Everything the engine produces -- a diagnostics list, a scene snapshot, a state-cell table, the canvas -- is
a **childless leaf React renders once** and a *session* fills:

| Session | Owns |
|---|---|
| `views/diagnostics-session.ts` + `views/diagnostics-render.ts` | the `tessera_diagnostics` module instance, the compile path, the list |
| `views/scene-session.ts` + `views/scene-render.ts` + `views/scene-input.ts` | the `tessera_scene_preview` instance, the snapshot and the click script |
| `views/canvas-session.ts` + `views/canvas-device.ts` + `views/canvas-pane.ts` | the `tessera_canvas` instance, the WebGPU device, the canvas surface and its frames |

The sessions are created once, in a ref, against the nodes React committed. They are never re-created by a
re-render, because each of them owns a Wasm instance, a GPU device or an animation-frame callback -- none of
which may be created by a render or destroyed by one. The React state is exactly what a visitor changes by
hand: the view, the toolbar, whether the gallery is open, the share status and the build id.

**The canvas is the load-bearing case.** `regions/CanvasPane.tsx` renders `<canvas>` as a childless leaf
with a ref: no `key`, no children, no conditional mount, and no `width`/`height` props (the session sizes the
backing store, because that is a device-pixel decision the module and the display share). React never
reconciles it again, so a preview crash loses the canvas rather than the editor, and React's render cycle
never runs while a GPU frame is being recorded.

`lib/views.ts` states the contract (`ViewSession`) and the routing (`?view=`, `history.replaceState`).

### 2.2 What stayed pure

**`share.js` and `preview-state.js` are still plain ES modules**, in `src/lib/`, with no DOM and no React
(ADR-054, Consequences). That is not a style choice: `scripts/ci/preview_share_roundtrip.mjs` imports them
**directly from source**, so the 19 share-link checks keep testing the same bytes the page imports rather
than a bundled copy. They carry `.d.ts` declarations (`share.d.ts`, `preview-state.d.ts`) so the React side
is typed without rewriting the codec. `scripts/ci/preview_page_checks.mjs` asserts the property from the
other side -- no DOM globals, no React import, relative specifiers only -- so an edit that broke it fails
there too.

The rest of the page is TypeScript:

| Module | Role |
|---|---|
| `main.tsx` | the browser entry point: mounts `App` into `#root` |
| `App.tsx` | the four regions, the shared state, the three sessions and their refs |
| `lib/preferences.ts` | the toolbar's persistence, the fragment decode, the share link (DOM-free) |
| `lib/gallery.ts` | the sample manifest and the sample fetch (DOM-free) |
| `lib/views.ts` | the `ViewSession` contract, the view table, `?view=` routing |
| `lib/wasm-call.ts` | the one C-ABI marshalling helper: allocate, call, `tessera_free`, free |
| `lib/module-url.ts` | resolves a Wasm loader against the document's base, so a hashed asset dir cannot break it |
| `views/dom.ts` | the textContent-only element builder the engine-facing renderers share |
| `ssr-chrome.tsx` | the chrome rendered with `react-dom/server`, for the Node gates only |
| `public/redirect.js` | the pre-merge URLs' forwarding (section 3) |
| `public/{scene,canvas}.html` | the redirect stubs |

The three views share the editor because that is the product claim: the same source is compiled, laid out
and painted without being copied between pages. The page, not a view, owns the Run button and the per-view
hint, so a tab switch cannot leave the previous view's sentence or a stale enabled state behind.

## 3. Old URLs and the share-link migration

`scene.html` and `canvas.html` still resolve. Each is a **redirect stub** in `designer/web/public/` (so Vite
copies it to the site root): a few dozen lines of HTML, the shared `redirect.js`, and no engine code and no
module import. A stub that had to fetch a module before forwarding would be a stub that can fail to forward,
and an old link is somebody's project state.

`redirect.js` builds

```text
index.html?<old query>&view=<scene|canvas><old fragment>
```

and calls `location.replace`, so the stub does not become a history step the visitor can get stuck on. Two
details are load-bearing:

- **The hash is concatenated, never re-serialized.** A fragment is opaque: a `#p5=` payload is base64url and
  may contain characters a `URL` round trip would re-encode. Assigning `url.hash` would risk loading
  different source than the one that was shared, which is the one failure a share feature must not have.
- **The query is carried, not read.** The old pages took viewport, DPR and theme from the fragment and from
  `localStorage`, never from the query (`initialState` in `lib/preferences.ts`). Forwarding preserves that: a
  query-only old link still opens at the default rectangle. Making the page start reading the query would
  restore a state the old link never had.

The view is not a fragment field -- `designer-studio-v1.md` section 7.1 freezes the envelope to `v`, `s`,
`w`, `h`, `d`, `t`, `f` -- which is exactly why the old *path* is translated into `?view=` at the stub
rather than guessed at on the merged page. The page's own `?view=` is read once at load and written with
`history.replaceState` on a tab click (replace, not push: a tab is not a navigation step).

Because `share.js` and `preview-state.js` are unchanged, a pre-merge link decodes to the same state by
construction. That is not left to inspection: `scripts/ci/preview_share_roundtrip.mjs` pins a fragment
**recorded from the pre-merge codec** as a fixture, so a future change to the envelope fails a test instead
of silently breaking links in circulation.

## 4. Layout

At and above 760 px the page is a two-pane split: the shared editor on the left, the active view on the
right. Below 760 px it is one scrolling column with a 16 px gutter -- the editor at a bounded height, then
the active view -- and no horizontal page scroll (`scrollWidth === clientWidth === 390` at a 390 px
viewport). The tabs stay one row. The phone rule is `display: block` rather than a two-row grid on purpose: a
fractional grid row collapses to nothing once the first row's content is taller than the viewport, which the
screenshot check caught.

`[hidden] { display: none !important; }` is in `app.css` because the panes are flex boxes: a user agent's
`[hidden]` rule loses to `.pane { display: flex }` on order, so without it a "hidden" pane would still take
its cell.

## 5. Gates, and the P6 budget

The gates moved with the stack (ADR-054 decision 6). React creates the DOM at runtime, so a static "every id
exists in the HTML" check no longer proves anything -- the shipped `index.html` is a mount point:

- `scripts/ci/preview_page_checks.mjs` reads the **built** output. `dist/index.html` must be CSP-clean and
  reference only assets that exist (derived from the document -- the fixed `test -f` list cannot survive
  content hashes); the chrome is rendered with `react-dom/server`'s `renderToStaticMarkup` from
  `dist-ssr/` and the ids/roles are asserted against that markup, per view; the six old-URL redirect shapes,
  the manifest paths, the two pure modules, the three lazy imports and the dangling-call audit are checked
  from the same run. `react-dom/server` is part of `react-dom`, so this added **no** dependency (jsdom
  would have needed a `dependencies.md` and `THIRD_PARTY_NOTICES.md` entry).
- `scripts/ci/preview_studio_smoke.mjs` drives the built `preferences`/`gallery` entries and the rendered
  chrome under a stub browser; the codec is still imported from source.
- `scripts/preview_budget_measure.py` walks the built `designer/web/dist/` as a **directory** instead of
  naming files, so an asset that ships is weighed by construction, and measures all three Wasm modules
  the studio loads (section 5.3).
- `.github/workflows/pages.yml` runs `npm ci` + `npm run build` in `designer/web` and assembles the site from
  `dist/` plus the three Wasm modules, the samples and the build id. The old `test -f` list is replaced by a
  scan of `dist/index.html`'s own asset references.
- `tests/golden/perf/preview_budget.json`: `download_gzip_bytes` re-recorded (section 5.2).

`preview_browser_check.mjs` and `preview_screenshots.mjs` are **developer-run** evidence, not CI gates: no
lane here has a browser. They add no dependency -- the DevTools Protocol over a `WebSocket`, with `node:`
builtins only -- because adding a browser dependency is a `docs/specs/dependencies.md` decision. Both run
against the assembled `build/pages`.

### 5.1 Why the download budget was raised for the merge (#392)

`download_gzip_bytes` went from 417327 to **436794** bytes (+4.7%): the deliberately updated ratchet that
issue's acceptance names. `--update` refuses a raise by design (a re-pin on a loaded box would record machine
load as headroom), so the pin is an explicit edit with this reason in the diff, recorded from the
measurement.

It is legitimate growth, not a regression:

- **~16911 bytes is the page.** The merged artifact ships *all three* views plus two redirect stubs, where
  the old artifact was one page and one module. The view modules and their render/pane helpers replace
  `app.js`/`scene.js`/`canvas.js`/`canvas.css`; `studio-page.js` and `shell.js` replace the three copies of
  header, toolbar and share wiring each separate page carried; `redirect.js` and the stubs are new and are
  the migration that issue exists for.
- **~2556 bytes is the diagnostics module, which the change did not touch.** It is byte-identical when
  rebuilt, so its gzip delta comes from the zlib build on the measuring host rather than from the merge. The
  recorded `encoder` matches this host; a re-measurement on the Pages lane may land a few kilobytes either
  way.

The two latency rows were left alone. Their values on this box moved with no C++ change at all, and the
ratchet's rule is that a warned wall-clock row is never re-pinned on whatever machine happened to measure it.
They still warn and still pass. (Section 5.3 is where they stopped warning: #395 gave them a number that
does not move with the box.) The `recorded` breakdown in the same entry was also left alone here, which
turned out to matter -- section 5.3 explains the 22180-byte gap that opened as a result.

### 5.2 Why the download budget was re-recorded for the conversion (#422)

`download_gzip_bytes` went from 436794 to **470568** bytes (+33774, +7.7%), again as a reviewed edit and
again recorded from the measurement
(`python -B scripts/check_preview_budget.py --check --size-only --module-dir build/wasm-webgpu/src/compiler/codegen`).
Two things changed at once, and the note in `tests/golden/perf/preview_budget.json` records the attribution:

- **The measurement moved.** It walks `dist/`, because a Vite build's assets carry content hashes
  (`assets/index-<hash>.js`) and no file list can name them.
- **The bytes moved, and they decompose exactly.** The page went from 57388 gzip bytes (21 hand-written
  files) to 91162 (7 files). Of the +33774:

  | Component | Δ gzip bytes |
  |---|---|
  | React 19.3.1 + react-dom 19.3.1 (the framework floor) | **+69007** |
  | the vanilla JavaScript it replaces | **-30211** |
  | CSS and HTML, both minified by the build | **-5022** |
  | **net** | **+33774** |

  The framework floor is **measured, not estimated**: a control `vite build`, from the same lockfile and the
  same Vite, of `createRoot(...).render(<p/>)` with nothing else in the graph, is 69007 gzip bytes of JS. The
  page's own JavaScript is therefore 82797 - 69007 = 13790 bytes, against 44001 bytes of hand-written modules
  before (`studio-page.js`, `shell.js`, `wasm-call.js`, the three views and their five helpers, `studio.js`,
  `gallery.js`, `share.js`, `preview-state.js`, `scene-input.js`). React's component model is cheaper than
  the `document.createElement` tree it replaces on this page. `app.css` went 5065 -> 2434 (minified) and
  `index.html` 3602 -> 1211 (a mount point instead of 210 lines of markup). The four public files
  (`redirect.js`, the two stubs, `samples.json`) are copied verbatim and did not move: 4720 bytes.

The diagnostics module is unchanged (379406 gzip bytes, the same two files), which is why the download moved
by exactly the page delta. The result is inside the 6 MB product budget (`designer-studio-v1.md` section
2.3) with 5.5 MB to spare. The pinned total and the pinned `recorded` breakdown then disagreed for the
next two landings; section 5.3 is where that was found and closed.

### 5.3 Why every module is now weighed, and what the latency rows became (#395)

Issue #395 finished what P6 deferred. Two things changed, and only one of them is a number.

**The download budget covers all three modules.** ADR-054 decision 3 ships three lazy `import()`s and the
studio loads all three: diagnostics for the default view, scene preview for the Scene tab, canvas for the
Canvas tab. Until now only diagnostics was weighed, and it is the *smallest* of the three -- so the
largest file the studio ever downloads was outside the budget. Measured on a local `wasm-webgpu` build:

| File | gzip bytes |
|---|---|
| `tessera_diagnostics.wasm` + `.js` | 379406 |
| `tessera_scene_preview.wasm` + `.js` | 565929 |
| `tessera_canvas.wasm` + `.js` | 1313831 |
| **the two newly weighed modules** | **1879760** |

`download_gzip_bytes` therefore goes from 492748 to **2372834** bytes (+1880086), and the growth
decomposes exactly:

| Component | Δ gzip bytes |
|---|---|
| `tessera_scene_preview` (never weighed before) | +565929 |
| `tessera_canvas` (never weighed before) | +1313831 |
| the page + diagnostics, re-measured on this host | +326 |
| **net** | **+1880086** |

The +326 is *not* new page code: it is the difference between this host's measurement of the page plus
diagnostics, which #387 had already pinned, and that pin. #392 and #422 both recorded the same
phenomenon -- the diagnostics module is byte-identical across those rebuilds and its gzip delta comes
from the zlib build on the measuring host. No page number was deliberately re-pinned here (#431 is
restructuring `designer/web`); the page rows inside the entry's `recorded` breakdown did move, because
that breakdown is now checked against the measurement and must agree with it.

**A stale breakdown was found on the way.** The old entry's `recorded` block summed to 470568 -- the
#422 pin -- while its own `budget` was 492748, the #387 pin. The font re-record of #387 had raised the
pin without updating the per-file table, so for two landings the file's attribution was 22180 bytes
short of its own total and nobody could have noticed. That is the failure `breakdown_drift` now closes:
the attribution can no longer disagree with the number it explains.

The budget stays **one entry on the total** rather than one per module, so a regression fails once rather
than three times, and every module's own size now rides in the entry's `recorded` breakdown. That
breakdown is **checked**, not decorative: `preview_budget_ratchet.breakdown_drift` fails when a recorded
file's size disagrees with the measurement, or when a file the measurement weighs is not named. The
attribution a reviewer reads is therefore the measurement that produced the pin -- `--update` regenerates
it from the same run -- instead of a table someone copied out of a terminal once.

**The latency rows are enforced, on a normalized number.** They warned because wall clock on a shared
runner is not reproducible: the eight Pages runs of 2026-11-16/17 measured `cold_engine_ready_ms` between
16.552 and 28.272 ms and `edit_compile_ms` between 0.083 and 0.223 ms with no relevant source change.
That is 1.7x and 2.7x of runner noise, and no band can both cover it and catch a regression.

Rather than abandon the measurement or pretend the noise away, the row changed what it measures. Every
probe process now times a fixed integer workload, and the enforced value is
`measured_ms * calibration_reference_ms / calibration_ms`: the reference box's speed cancels, so the pin
means "this much time on the recorded reference profile". Measured on one box over 15 fresh processes,
the raw spread was 1.06x idle and 1.08x with twelve CPU-burner processes on twelve threads; the
normalized spread was 1.04x in both. The calibration absorbs contention because the loop and the compile
share a core, a governor and a process.

This is a real claim with a real limit, so it is written where a reviewer can attack it: it assumes the
*ratio* of integer-arithmetic speed to Wasm-compile speed is roughly constant across x86-64 runner
generations, and only one host was measured. Both sides of the band are wide for that unmeasured
component (`worse` 100%, `better` 50% -- against 5%/1% for the deterministic size row): the ratios are
not symmetric in risk, because a host whose compile speed *relative to arithmetic* is 60% worse is a
real possibility while a 20% improvement is what a genuine code win looks like, and the better side
still has to be tight enough to notice one. Every run prints `cold_engine_ready_raw_ms`,
`edit_compile_raw_ms` and `calibration_ms` so a human can tell "the code got slower" from "the runner
class changed". If a later lane shows the ratio is not stable across hosts, the honest response is to
demote the two rows back to `warn` and ratchet a work counter instead: the ratchet code does not change,
so that is a `status` edit and a paragraph here.

**What was deliberately not done.** The page numbers were not re-pinned (#431 moves them), and the two
new modules' sizes were *not* folded into the page measurement -- they stay measured from the module
directory, because they are linked engine artifacts the Pages lane copies next to the page, never
bundled.

**How a re-record happens.** Two mechanisms, and they are different on purpose. A **tightening** is
`python -B scripts/check_preview_budget.py --update`, which lowers a pin and refreshes the `recorded`
breakdown from the same run. A **raise** is a reviewed edit of the pin in the diff, with the reason
recorded in the budget file's `note` -- `--update` refuses any raise, because a re-pin on a loaded box
records machine load as headroom. `measurement.calibration_reference_ms` gets the same treatment as
`encoder`: changing it rebases every latency pin at once, so it is a reviewed edit, never a side effect.

## 6. What this does not do

Out of scope by #392 and #422: author CSS (#390), the folder IDE and file tree (#391), pointer input on the
canvas (#388), text drawing (#387), conditional children (#389), skins and picker (#171). #422 added **no
studio feature**: it changed the shell's implementation, not the feature set. The Wasm builds stay three
modules. The studio stays single-threaded (ADR-045 decision 5): no threads, no `SharedArrayBuffer`, no
cross-origin-isolation service worker, and every page-to-engine call stays a message with transferable
buffers, so the owner-planned multi-threaded studio site can reuse the host code unchanged.
