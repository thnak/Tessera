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
  naming files, so an asset that ships is weighed by construction.
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
They still warn and still pass.

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
2.3) with 5.5 MB to spare.

## 6. What this does not do

Out of scope by #392 and #422: author CSS (#390), the folder IDE and file tree (#391), pointer input on the
canvas (#388), text drawing (#387), conditional children (#389), skins and picker (#171). #422 added **no
studio feature**: it changed the shell's implementation, not the feature set. The Wasm builds stay three
modules. The studio stays single-threaded (ADR-045 decision 5): no threads, no `SharedArrayBuffer`, no
cross-origin-isolation service worker, and every page-to-engine call stays a message with transferable
buffers, so the owner-planned multi-threaded studio site can reuse the host code unchanged.
