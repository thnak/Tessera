# The merged studio page (issue #392)

Status: landed (Big Step D, V1-M4). Issue: [#392](https://github.com/thnak/Tessera/issues/392).
Spec: [`designer-studio-v1.md`](../specs/designer-studio-v1.md) sections 3 and 7.1.
Predecessors: [`compiler-wasm.md`](compiler-wasm.md) sections 5-10 (P1 diagnostics, P2/P3 scene and
canvas, P4 input, P5 share links, P6 budgets) and
[`reference-equantic-ui.md`](reference-equantic-ui.md) section 7.4.

The preview used to be three pages. This note records the merge into one `index.html`: what the page
is made of, how each of the three previews is reached, how a pre-merge URL still works, and why the
P6 download budget was re-pinned.

## 1. One page, three views, one set of controls

`index.html` has exactly one of each shared control -- a header and subtitle, a toolbar (viewport, DPR,
theme), a sample gallery, a "Copy share link" button, a source editor, a Run button and a Load-sample
button -- and three **views** that swap inside one pane:

| View | What it shows | Its module | When it loads |
|---|---|---|---|
| Diagnostics | the diagnostics list with source spans | `tessera_diagnostics` | at first paint (the default view) |
| Scene | the scene snapshot plus the `<automationId>` click script and its state cells | `tessera_scene_preview` | when the Scene tab is first opened |
| Canvas | the evaluated scene painted through WebGPU, with the frame counters | `tessera_canvas` | when the Canvas tab is first opened (device on the first Run there) |

The three Wasm builds are **not** merged; this issue is the page merge only. Keeping them separate is
also what keeps the failure modes separate: a browser without WebGPU keeps Diagnostics and Scene and
sees a named unsupported state in Canvas only, and a module that fails to import leaves its own pane
unavailable rather than the page blank (`reference-equantic-ui.md` section 7.5).

**Laziness is the constraint the merge had to satisfy.** Diagnostics is the default view and the only
module on the first-paint path, so `cold_engine_ready_ms` and the first-paint request set do not grow:
the measured 15.4 ms sits inside the unchanged P6 budget. A view's module is imported by a dynamic
`import()` when its tab is first shown, and the canvas view additionally defers its
`requestAdapter`/`requestDevice` to the first Run on that view -- a visitor who never opens Canvas never
downloads `tessera_canvas.wasm` and never takes a GPU.

## 2. Module layout

One ES module per responsibility, all vanilla JS with no framework, no bundler and no build step, and
each under the 300-line soft limit:

| Module | Role |
|---|---|
| `studio-page.js` | entry point: mounts the chrome and the shell, mounts each view on first use, routes Run / Load sample / toolbar / gallery to the active view, fetches the build id once |
| `shell.js` | the view tablist (roving tabindex, arrow keys), the `?view=` parameter, the per-view subtitle and editor hint |
| `view-diagnostics.js` + `diagnostics-view-render.js` | the diagnostics view and its list rendering |
| `view-scene.js` + `scene-view-render.js` | the scene view (snapshot and input script) and its node rendering |
| `view-canvas.js` + `canvas-device.js` + `canvas-pane.js` | the canvas draw path, the WebGPU handover, and the four pane states (unsupported / failed / frame / idle) |
| `wasm-call.js` | the one C-ABI marshalling helper: allocate, call, `tessera_free`, free |
| `redirect.js` | the pre-merge URLs' forwarding (section 3) |

`studio.js` (toolbar, gallery, share link, local preferences), `gallery.js`, `share.js`,
`preview-state.js` and `scene-input.js` are **unchanged**: the merge reused the P5 chrome rather than
rewriting it. That is why the share-link codec is byte-identical across the merge, which is the
foundation of section 3's guarantee.

The three views share the editor because that is the product claim: the same source is compiled, laid
out and painted without being copied between pages. The page, not a view, owns the Run button and the
per-view hint, so a tab switch cannot leave the previous view's sentence or a stale enabled state
behind.

## 3. Old URLs and the share-link migration

`scene.html` and `canvas.html` still resolve. Each is a **redirect stub**: a few dozen lines of HTML,
the shared `redirect.js`, and no engine code and no module import. A stub that had to fetch a module
before forwarding would be a stub that can fail to forward, and an old link is somebody's project
state.

`redirect.js` builds

```text
index.html?<old query>&view=<scene|canvas><old fragment>
```

and calls `location.replace`, so the stub does not become a history step the visitor can get stuck on.
Two details are load-bearing:

- **The hash is concatenated, never re-serialized.** A fragment is opaque: a `#p5=` payload is
  base64url and may contain characters a `URL` round trip would re-encode. Assigning `url.hash` would
  risk loading different source than the one that was shared, which is the one failure a share feature
  must not have.
- **The query is carried, not read.** The old pages took viewport, DPR and theme from the fragment and
  from `localStorage`, never from the query (`initialState` in `studio.js`). Forwarding preserves that:
  a query-only old link still opens at the default rectangle. Making the merged page start reading the
  query would restore a state the old link never had.

The view is not a fragment field -- `designer-studio-v1.md` section 7.1 freezes the envelope to `v`,
`s`, `w`, `h`, `d`, `t`, `f` -- which is exactly why the old *path* is translated into `?view=` at the
stub rather than guessed at on the merged page. The merged page's own `?view=` is read once at load and
written with `history.replaceState` on a tab click (replace, not push: a tab is not a navigation step).

Because `share.js` and `preview-state.js` are unchanged, a pre-merge link decodes to the same state by
construction. That is not left to inspection:
`scripts/ci/preview_share_roundtrip.mjs` pins a fragment **recorded from the pre-merge codec** as a
fixture, so a future change to the envelope fails a test instead of silently breaking links in
circulation.

## 4. Layout

At and above 760 px the page is a two-pane split: the shared editor on the left, the active view on the
right. Below 760 px it is one scrolling column with a 16 px gutter -- the editor at a bounded height,
then the active view -- and no horizontal page scroll
(`scrollWidth === clientWidth === 390` at a 390 px viewport). The tabs stay one row. The phone rule is
`display: block` rather than a two-row grid on purpose: a fractional grid row collapses to nothing once
the first row's content is taller than the viewport, which the screenshot check caught.

`[hidden] { display: none !important; }` is in `app.css` because the panes are flex boxes: a user
agent's `[hidden]` rule loses to `.pane { display: flex }` on order, so without it a "hidden" pane would
still take its cell.

## 5. Gates, and the P6 budget

Updated with the merge, because a page asset that is not listed ships unweighed:

- `scripts/preview_budget_measure.py` (`PAGE_FILES`): the merged page's files, the two stubs and
  `redirect.js`; `app.js`/`scene.js`/`canvas.js`/`canvas.css` removed.
- `.github/workflows/pages.yml`: the `test -f` list for the new files and the stubs.
- `scripts/ci/preview_page_checks.mjs`: ids checked against one page plus each view's own list, the
  redirect's query/fragment preservation for six old-URL shapes, and no dangling calls across twelve
  modules.
- `tests/golden/perf/preview_budget.json`: `download_gzip_bytes` re-pinned (section 5.1).

`preview_browser_check.mjs` and `preview_screenshots.mjs` are **developer-run** evidence, not CI gates:
no lane here has a browser. They add no dependency -- the DevTools Protocol over a `WebSocket`, with
`node:` builtins only -- because adding a browser dependency is a `docs/specs/dependencies.md` decision.

### 5.1 Why the download budget was raised

`download_gzip_bytes` went from 417327 to **436794** bytes (+4.7%): the deliberately updated ratchet the
issue's acceptance names. `--update` refuses a raise by design (a re-pin on a loaded box would record
machine load as headroom), so the pin is an explicit edit with this reason in the diff, recorded from
the measurement.

It is legitimate growth, not a regression:

- **~16911 bytes is the page.** The merged artifact ships *all three* views plus two redirect stubs,
  where the old artifact was one page and one module. The view modules and their render/pane helpers
  replace `app.js`/`scene.js`/`canvas.js`/`canvas.css`; `studio-page.js` and `shell.js` replace the
  three copies of header, toolbar and share wiring each separate page carried; `redirect.js` and the
  stubs are new and are the migration this issue exists for.
- **~2556 bytes is the diagnostics module, which this change did not touch.** It is byte-identical when
  rebuilt (`SHA-256 7A6FF0AD…095E7F` on both builds), so its gzip delta comes from the zlib build on
  the measuring host rather than from the merge. The recorded `encoder` matches this host; a
  re-measurement on the Pages lane may land a few kilobytes either way.

The two latency rows were left alone. Their values on this box (`edit_compile_ms` 0.044 against a 0.036
pin) moved with no C++ change at all, and the ratchet's rule is that a warned wall-clock row is never
re-pinned on whatever machine happened to measure it. They still warn and still pass.

## 6. What this does not do

Out of scope by the issue: author CSS (#390), the folder IDE and file tree (#391), pointer input on the
canvas (#388), text drawing (#387), conditional children (#389), skins and picker (#171). The Wasm
builds stay three modules. The studio stays single-threaded (ADR-045 decision 5): no threads, no
`SharedArrayBuffer`, no cross-origin-isolation service worker, and every page-to-engine call stays a
message with transferable buffers, so the owner-planned multi-threaded studio site can reuse the host
code unchanged.
