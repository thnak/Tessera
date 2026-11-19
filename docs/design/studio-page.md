# The hosted studio page (issues #392, #422 and #431)

Status: landed (Big Step D, V1-M4). Issues: [#392](https://github.com/thnak/Tessera/issues/392) (the three-
page merge), [#422](https://github.com/thnak/Tessera/issues/422) (the React 19 + Vite conversion) and
[#431](https://github.com/thnak/Tessera/issues/431) (the one-workspace layout: project tree, tabbed code,
preview column).
Decision: [ADR-054](../../decisions/ADR-054-studio-front-end-react-vite.md) (the conversion),
[ADR-045](../../decisions/ADR-045-browser-hosted-studio.md) (the browser-hosted studio, whose page-stack
wording ADR-054 amends and whose decisions stand).
Spec: [`designer-studio-v1.md`](../specs/designer-studio-v1.md) sections 2, 3, 7.1 and 10.
Predecessors: [`compiler-wasm.md`](compiler-wasm.md) sections 5-10 (P1 diagnostics, P2/P3 scene and canvas,
P4 input, P5 environment toggles, P6 budgets), [`studio-project-io.md`](studio-project-io.md) (the zip container and
the path-confinement rule, which #431 is the first slice to use in the browser) and
[`reference-equantic-ui.md`](reference-equantic-ui.md) section 7.4.

The preview used to be three pages. #392 merged them into one `index.html`; #422 converted that page's
chrome from hand-written vanilla ES modules to **React 19 + TypeScript, built by Vite**; #431 made it **one
workspace** instead of three top-level tabs. This note records what the page is made of, how each preview is
reached, how a pre-merge URL still works, where the boundary between React and the engine sits, and why the
P6 download budget was re-recorded three times.

## 1. One workspace, three columns, three previews

The page is a single workspace and it always looks the same:

```text
+--------------+---------------------------+---------------------------+
| project tree |  code (a tab per open file)|  preview                 |
|  ui/         |  [App.tsx] [theme.css]    |  [scene] [canvas] [diag]  |
|    App.tsx   |                           |                           |
|  styles/     |  the editor               |  the device frame         |
+--------------+---------------------------+---------------------------+
| status bar: problems, line:col, language, bell                         |
+-----------------------------------------------------------------------+
```

The three preview views are **panes inside the right-hand column**, not three top-level tabs:

| View | What it shows | Its Wasm module | When it loads |
|---|---|---|---|
| Diagnostics | the diagnostics list with source spans | `tessera_diagnostics` | at first paint (the default view) |
| Scene | the scene snapshot plus the `<automationId>` click script and its state cells | `tessera_scene_preview` | when the Scene tab is first shown |
| Canvas | the evaluated scene painted through WebGPU, with the frame counters and the running evaluation's state cells | `tessera_canvas` | when the Canvas tab is first shown (the device is acquired by the first *apply* there, which since #444 is the automatic one) |

**No view switch changes the workspace's shape.** That is #431's acceptance sentence. Since W5 (#560) the
workspace is the dock workbench ([`studio-workbench-dock.md`](studio-workbench-dock.md)): the views are
panels (`regions/PreviewPanels.tsx`) in the preview group's tab strip, and the tree and the editor do not
move, resize or re-render when the active view changes; `scripts/ci/preview_browser_layout.mjs` still
measures their boxes before and after a switch.

The three Wasm builds are **not** merged, by any of the three issues. Keeping them separate is also what
keeps the failure modes separate: a browser without WebGPU keeps Diagnostics and Scene and sees a named
unsupported state in Canvas only, and a module that fails to import leaves its own pane unavailable rather
than the page blank ([`reference-equantic-ui.md`](reference-equantic-ui.md) section 7.5).

**Laziness is the constraint all three issues had to satisfy.** Diagnostics is the default view and the only
module on the first-paint path. A view's Wasm module is imported by a dynamic `import()` when its tab is
first shown, and the canvas view additionally defers its `requestAdapter`/`requestDevice` to the first
**apply** on that view -- since #444 that is the automatic one rather than a button, but it is still not page
load: a visitor who never opens Canvas never downloads `tessera_canvas.wasm` and never takes a GPU.
`scripts/ci/preview_page_checks.mjs` asserts that each of the three module URLs appears exactly once in the
built bundle, inside a dynamic import, and the browser-verify run records the request log.

**Live edit (issue #444).** The developer does not press Run. One controller
(`designer/web/src/page-live.ts`) owns the page's only debounce timer, and every path that changes the
source goes through it:

- a keystroke schedules an apply 150 ms after the burst ends (`schedule`);
- `Ctrl+Enter` cancels the timer and applies immediately (`applyNow`);
- **Restart app** (`run.restart`: toolbar, Run menu) cold-starts every mounted pane (`restart`) -- the explicit state reset, and the only
  thing on the page that discards state.

The apply is not a restart. Each session's `apply()` re-evaluates carrying state by the engine's own rule
(component id, cell id, IR type -- `scene_state.hpp`), and the Canvas session applies the source and then
paints it as two module calls (`tessera_canvas_apply` then `tessera_canvas_draw`) rather than one stateless
call. That split is what makes the two hard requirements hold: an apply of text that did
not change reports `changed:false` and costs **no frame**, and an apply that does not compile leaves the
running evaluation and its picture untouched -- the page shows the diagnostic and draws nothing, so the
canvas cannot blank mid-keystroke. The old one-shot path (now removed) was the blanking mechanism: it
re-evaluated per call and called `clear_canvas` on any failure.

Two changes restart instead of applying, both because the state cannot survive them: a **structural change
that removes the node a cell lives on** (the engine reports it as `droppedComponent`), and a **change to a
project file that is not a source file** (a config, a theme). The trigger notices the second from the
edited file's `FILE_KIND` before it starts the debounce.

The 150 ms timer and the pane's `requestFrame()` compose rather than duplicate: the timer turns a burst of
keystrokes into one apply, and `requestFrame()` turns a burst of applies into one frame (it returns while a
frame is already pending). Neither re-arms itself, so a static scene draws zero frames after the edit
settles. The Canvas pane's frame counter is the engine's own `frames_painted()`, surfaced in the counters
and the status line so the rule is observable rather than asserted. `#stat-state` shows the running
evaluation's cells, which is how a developer sees a carried `count` -- and sees it go back to its
initializer after **Restart app**.

`scripts/ci/preview_browser_live_edit.mjs` is the developer-run acceptance probe for those six sentences
(one frame per burst, zero when static, state carried, syntax error keeps the frame, Restart resets it,
all three panes from one edit). `scripts/ci/preview_browser_css_live_edit.mjs` adds the case that belongs to
neither feature alone: with the author sheet applied (section 3.4), a source edit keeps the author's
styling, because the sheet travels on `tessera_canvas_apply` rather than on `tessera_canvas_draw`.

**Device and orientation changes are layout inputs, not re-evaluations (issue #436).** The Viewport and
DPR selects do not make the Canvas view re-run the source: the session calls
`tessera_canvas_set_viewport`, which re-lays the running evaluation out in place -- one pass, no
evaluation -- and re-resolves `@media` only when the size crosses a breakpoint. So `#stat-state` keeps
its cells and hover/focus survive a preset switch, and the frame counter moves by exactly one. The Scene
view is a document per call, so its own re-run already answered for the new rectangle; what it could not
do before #436 was resolve `@media` for it at all (the sheet kept `MediaContext`'s default 1280x720).
`scripts/ci/preview_browser_viewport.mjs` is the developer-run probe for colour, state and frames.

### 1.1 Skins and the simulated device frame (issue #171's skins slice)

`designer-studio-v1.md` section 5.2 assigns the bundled skins to #171, and this slice lands them. A skin is
**data plus static art, never code**: one directory under `designer/web/public/skins/` per skin, each
holding a `skin.json` matching section 5.2's manifest and, optionally, an SVG frame. The seven bundled skins
are the spec's set (generic desktop Windows 11 / macOS / Linux, generic phone Android / iOS, generic tablet,
generic foldable), and **all art is original** -- the one SVG frame (`generic-phone-android/frame.svg`) was
drawn for this repository; no vendor device art is redistributed.

**The geometry contract (section 5.1) is one derivation, in one file.** `views/skin-geometry.ts` turns a
validated manifest into the viewer size (`size` minus `bezel`), the `safeAreas` the engine pads by, the
cutout rectangle and the bars a mode draws. Nothing else computes an inset, so "the size in `viewport.set`
is the size the preview renders at" is checkable by reading one module.

**The insets reach the engine, not only the picture.** `tessera_canvas_set_safe_area(insets, cutout)` is a
new export beside `tessera_canvas_set_viewport` (the old three-argument form is untouched, so a page built
against the previous module keeps working); it fills `WindowMetrics::insets` and `WindowMetrics::cutouts`
through `simulated_metrics()`, so `hal::resolve_safe_area()` pads a `SafeArea` by exactly what the manifest
declared. A page-only overlay would have drawn a phone's status bar over a tree laid out as if the bar were
not there -- a skin that changes the picture without changing the layout, which section 5.1 forbids. The
pass completes inside the call, so the next `tessera_canvas_draw` paints the new insets on its first frame.

**Chrome colours are not part of the skin.** The frame paints from the page's own `--bg`/`--text`/`--border`
tokens (`styles/skin.css`), and the SVG frame is a CSS **mask** rather than an `<img>`, so it is painted in
`currentColor`. The Theme select now writes `<html data-theme>` (`page-skins.ts`), which is what makes the
retint rule demonstrable: one cascade moves the frame and the canvas together. Before this slice the Theme
control was a stored value and changed nothing on the page.
## 2. The stack, and the React/engine boundary

`designer/web` is a **Vite + React 19 + TypeScript** project (ADR-054 decisions 1-4):

- `package.json` / `package-lock.json` (committed) / `vite.config.ts` / `vite.ssr.config.ts` /
  `tsconfig.json`; `dist/` and `dist-ssr/` are git-ignored and never committed.
- `npm run build` is `tsc --noEmit` + `vite build` (the site) + `vite build --config vite.ssr.config.ts`
  (the gate-only bundle). The typecheck is part of the build: a type error is a build failure, not a runtime
  surprise in a browser no CI lane can drive.
- Styles are **seven static stylesheets split by region** (`src/styles/base.css`, `chrome.css`,
  `workspace.css`, `code.css`, `preview.css`, `skin.css`, `responsive.css`), imported by `src/main.tsx` in
  cascade order. Vite emits **one** content-hashed `assets/index-<hash>.css`, so the CSP ("no inline style,
  no remote style") and the download budget still see one file. #431 split the sheet because the
  three-column layout pushed the single `app.css` past the 500-line hard limit; #171's skins slice added
  `skin.css` for the same reason.
- The built `index.html` carries one `<script type="module" src="...">` and one `<link rel="stylesheet">`,
  both same-origin and content-hashed: **no inline script, no remote script** (ADR-045 decisions 1-3,
  ADR-054 decision 4).
- React is bundled into the static page and is never linked into an app binary or the Wasm engine
  (ADR-024, AGENTS.md non-negotiable 7). Node/npm is design-time tooling only.

### 2.1 What React renders, and what it does not

**React renders the studio chrome only** (ADR-054 decision 3). The engine still draws exactly one thing --
the preview canvas -- and the page remains its host.

| Region (`designer-studio-v1.md` section 3) | Component |
|---|---|
| studio chrome (title bar, menu bar, toolbar, device chip; issue #557) | `regions/WorkbenchChrome.tsx`, `regions/MenuBar.tsx`, driven by `page-workbench.ts` ([core](studio-workbench-core.md)); `regions/GalleryRegion.tsx` |
| the project tree (left column) | `regions/ProjectTree.tsx`, over `lib/project.ts` |
| the code column (middle: tabs, editor) | `regions/CodeRegion.tsx` |
| the preview column (right: the view switch, the three panes; the device picker is the Device menu) | `regions/PreviewColumn.tsx`, `regions/ViewPanes.tsx`, `regions/CanvasPane.tsx` |
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
hand: the workspace (`page-workspace.ts`), the toolbar and the build id.

**The canvas is the load-bearing case.** `regions/CanvasPane.tsx` renders `<canvas>` as a childless leaf
with a ref: no `key`, no children, no conditional mount, and no changing `width`/`height` props (the session
sizes the backing store, because that is a device-pixel decision the module and the display share). React
never reconciles it again, so a preview crash loses the canvas rather than the editor, and React's render
cycle never runs while a GPU frame is being recorded. #431 moved the pane into a column and did not change
any of that: the three panes are `hidden`, never unmounted, so a view switch keeps the device and the Wasm
instances alive.

`lib/views.ts` states the contract (`ViewSession`, `ProjectReader`) and the routing (`?view=`,
`history.replaceState`).

### 2.2 What stayed pure

**`preview-state.js` is still a plain ES module**, in `src/lib/`, with no DOM and no React (ADR-054,
Consequences; the share codec that used to sit beside it was removed with the Share feature). That is not a
style choice: the Node gates import it **directly from source**, so they keep testing the same bytes the page
imports rather than a bundled copy. It carries a `.d.ts` declaration (`preview-state.d.ts`) so the React side
is typed. `scripts/ci/preview_page_checks.mjs` asserts the property from the other side -- no DOM globals, no
React import, relative specifiers only -- so an edit that broke it fails there too.

`project-paths.js` and `studio-zip.js` keep the same rule for the same reason: no imports, no DOM, no
`window` ([`studio-project-io.md`](studio-project-io.md)). #431 is the first slice that imports
`project-paths.js` into the **shipped bundle** (the tree must not show a path the compiler would resolve
differently); that costs 2381 minified bytes and is why the P6 budget moved again (section 6).

The rest of the page is TypeScript:

| Module | Role |
|---|---|
| `main.tsx` | the browser entry point: mounts `App` into `#root`, imports the six stylesheets |
| `App.tsx` | the five regions, the shared state, the three sessions and their refs |
| `page-workspace.ts` | the workspace's state: the project, the open tabs, the active file, the selection, the dirty set, and the per-file buffer map |
| `lib/project.ts` | the seeded project, `ProjectFile`/`TreeRow`, `treeRows`, `compileTarget`; every path goes through `project-paths.js` |
| `lib/preferences.ts` | the toolbar's persistence (DOM-free) |
| `lib/gallery.ts` | the sample manifest and the sample fetch (DOM-free) |
| `lib/views.ts` | the `ViewSession`/`ProjectReader` contract, the view table, `?view=` routing |
| `lib/wasm-call.ts` | the one C-ABI marshalling helper: allocate, call, `tessera_free`, free |
| `lib/module-url.ts` | resolves a Wasm loader against the document's base, so a hashed asset dir cannot break it |
| `views/dom.ts` | the textContent-only element builder the engine-facing renderers share |
| `ssr-chrome.tsx` | the chrome rendered with `react-dom/server`, for the Node gates only |
| `public/redirect.js` | the pre-merge URLs' forwarding (section 3) |
| `public/{scene,canvas}.html` | the redirect stubs |

## 3. The workspace: tree, tabs, and what an edit compiles

### 3.1 The project tree (left)

The tree lists the files of the opened project. #431's deliverable is the **rendering and the selection**;
the two real load paths (a folder through File System Access, a dropped zip) are wired in
[`studio-project-io.md`](studio-project-io.md) section 5. The page still boots with a **seeded in-memory project**
(`lib/project.ts`, `SEED`): `tessera.json` (`app.entry` names `ui/App.tsx`), `ui/App.tsx` (the entry, which
imports `../styles/theme.css`) and `styles/theme.css`. There is no router yet: the scene builder does not
render user component instances, so a second screen cannot be composed in until the engine can. The sample gallery still works and adds a sample as a project file at
its own path, so the tree shows where it came from.

**Every path the tree shows came from `project-paths.js`.** `projectFile()` calls `confinePath` on each
path, then `classifyPath` to decide the kind, and `isTextKind` to decide whether the editor may open it --
which is the spec's accepted-file rule made visible: a font or an image is listed with a `locked` badge and
never read, and a path that escapes the project is refused by that module's code rather than filtered by a
second rule here. `treeRows()` derives the directories from the file paths (a zip carries no empty
directory) and returns a flat, depth-annotated list.

The tree is `role="tree"` with one `tabIndex=0` row (the selection), every row a `role="treeitem"` with
`aria-level`/`aria-selected` and -- for a directory -- `aria-expanded`. Arrow keys move selection and focus
together; `ArrowRight` opens a collapsed directory or steps into an open one, `ArrowLeft` closes it or steps
out to the parent, `Home`/`End` jump. A directory row toggles on click; a file row selects, becomes the
active file and gets a tab.

### 3.2 The code column (middle)

One tab per open file, a real `role="tablist"` with a roving tabindex and `ArrowLeft`/`ArrowRight`/`Home`/
`End` inside it; `Delete` (or `Ctrl+W`) closes the focused tab. Each tab carries a kind letter (`tsx`, `css`,
`json`), the file name, an unsaved dot and a close button *beside* the tab rather than inside it (interactive
content inside a `role="tab"` is a broken control).

**The buffers are not React state.** `page-workspace.ts` keeps a `Map<path, text>` in a ref and writes the
editor's document from an effect when the active file changes; a keystroke goes to `writeBuffer`, which
compares against the project's copy and re-renders **only when the dirty state changes**. Putting the text
in state would re-render the tree, the tabs, the preview column and the canvas host on every character. A
keystroke also calls the live-edit trigger (section 1), which is what makes the preview follow the text
without a button.

**The editor is CodeMirror 6 (issue #517).** It was a `<textarea>` while #431 kept the no-new-dependency
rule; the owner then chose CodeMirror (2026-11-18), already listed in `docs/specs/dependencies.md`. Everything
CodeMirror-specific is in `src/editor/`: `code-editor.ts` is a small handle (`open`, `text`, `reveal`,
`setMarks`, `focus`) and `editor-theme.ts` maps token tags to CSS custom properties, so the palette is two
blocks in `styles/code.css` (`--ed-*`, `--tok-*`, light and dark, every pair at least 4.5:1). Each open file
keeps its own `EditorState`, so **undo survives a tab switch**; a page write (a sample, a restored draft) is
told apart from the person's own history by comparing the stored document with the text the page hands
over. The compiler's diagnostics are underlined at their span (a wavy line in the severity colour, the
message as the tooltip) by `diagnostics-session.ts`, next to the unchanged list. Mod-S and Mod-Enter are the
editor's keymap (save, apply now). Tab is *not* bound to indent, so the keyboard can leave the editor.

Three consequences to remember. The editor mounts in an effect, so the server-rendered chrome carries
`#editor-host` and the live surface carries `#source` (the old id, on the editable node). The probes read and
type through the page's test seam `window.__tesseraEditor` (`text()`, `edit(text)`) because a CodeMirror
surface has no `value`. And CodeMirror injects its own `<style>` rules at run time; if the page ever ships a
strict style CSP, pass a nonce (`EditorView.cspNonce`) rather than loosening the policy.

### 3.3 What an edit compiles

The rule is **the whole project, from the entry `tessera.json` names** (`entryOf()` in
`lib/project-snapshot.ts`). The active file does not decide what is previewed: selecting
`styles/theme.css` edits that sheet, and the preview keeps running the entry. The status bar's language cell
names the entry in its tooltip, so the choice is visible rather than surprising.

The sessions no longer read `#source.value`: the page hands each of them a `ProjectReader` that returns
`{ entry, sources, stylesheets, active }` -- the whole project and the entry `tessera.json` names
(`lib/project-snapshot.ts`), read at apply time through a ref. Each session stages it into its module
(`stageProject()` in `lib/wasm-call.ts`) and then calls the export. That keeps the Wasm sessions
independent of React's render cycle, and lets Diagnostics judge each span on its own file against the file
the editor shows (a span in another file names the file instead of offering a jump it cannot make). There
is no fallback entry: a project whose `tessera.json` names none previews nothing and says why.

### 3.4 The author stylesheet (issue #390)

There is no stylesheet box: a stylesheet is a project file (`styles/theme.css` in the seed), edited in a tab
like any other, and it applies **only because the entry imports it** (`import "../styles/theme.css";`),
exactly as a web import graph cascades. A sheet that is not imported does nothing, and the first imported
sheet carrying any rule **replaces** the built-in preview sheet (owner decision 2); it is never layered under
it. Only when no imported sheet has a rule does the built-in sheet stay in force, byte-for-byte, which keeps
the bundled samples painting. A sheet with one *bad* rule still counts as the author's, so an invalid
declaration cannot resurrect the built-in look while the page shows the error.

**Where the sheets reach the sessions.** They travel in the `ProjectReader` snapshot with the sources
(section 3.3), staged by `stageProject()`, so a session cannot lay the tree out against a sheet the author no
longer has on screen. Scene calls `tessera_scene_json` and asks `tessera_css_diagnostics_json` for the parse
problems (`cssVersion: 2`: each problem names its file). Canvas passes the same staged project to
**`tessera_canvas_apply`**, the call that re-evaluates; `tessera_canvas_draw` takes no strings, because it
only paints the tree the last apply left. Both reports carry the same `cssDiagnostics` array, so a bad rule
costs that rule and never blanks the frame -- including on the live-edit path. Editing a stylesheet is a
live apply, like a source edit; editing `tessera.json` (it names the entry) restarts.

**No share link.** A whole project, or even one document, cannot be shared: the studio has no cloud storage
to share to, and the Share feature (button, `file.share`, the `#p5=` codec) was removed.

## 4. Old URLs

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

- **The hash is concatenated, never re-serialized.** A fragment is opaque: an old `#p5=` payload is base64url
  and may contain characters a `URL` round trip would re-encode, so the stub passes it through byte for byte.
- **The query is carried, not read.** Forwarding never starts reading it: an old link opens at the stored
  toolbar position, or the default rectangle.

The view is not a fragment field, which is exactly why the old *path* is translated into `?view=` at the stub
rather than guessed at on the merged page. The page reads `?view=` once at load and writes it with
`history.replaceState` on a switch (replace, not push: a view switch is not a navigation step). **#431 kept
the parameter and every id it resolves** (`tab-scene`, `view-canvas`, ...), so a link made against the
three-tab page opens on the same preview pane it names.

**A `#p5=` link no longer restores state.** The share codec was removed (owner decision, studio focus pass), so
an old fragment is carried by the stub, kept in the address bar and ignored by the page: no decode, no banner,
no error. `scripts/ci/preview_page_checks.mjs` still pins the six old-URL shapes the stubs forward, and
`preview_studio_smoke.mjs` asserts that a leftover fragment leaves the startup state at the stored toolbar or
the default.

## 5. Layout and gating

### 5.1 The three states

| Width | Layout |
|---|---|
| `>= 1200px` | three columns: a 232 px tree rail, the code column, the preview column; the status line spans the bottom |
| `760-1199px` | two columns: the tree rail and the code column side by side, the preview spanning beneath them |
| `< 720px` | superseded by W5: one visible dock group with a tab switcher (`phoneLayout()`) |

The phone rule is `display: block` rather than a shrinking grid, and the flexible tracks are
`minmax(0, ...)`: a grid item's default `min-width: auto` lets a `wrap="off"` textarea or a 1280-px canvas
push its track wider than the page, which is exactly the horizontal scroll the acceptance forbids.
`scripts/ci/preview_browser_layout.mjs` measures `document.documentElement.scrollWidth <= clientWidth` at
390x844 (it reports `390 vs 390`) and confirms the preview is reachable in the single column.

`[hidden] { display: none !important; }` is still in `base.css` because the panes are flex boxes: a user
agent's `[hidden]` rule loses to `.pane { display: flex }` on order, so without it a "hidden" pane would
still take its cell.

### 5.2 Gates

The gates moved with the stack (ADR-054 decision 6). React creates the DOM at runtime, so a static "every id
exists in the HTML" check no longer proves anything -- the shipped `index.html` is a mount point:

- `scripts/ci/preview_page_checks.mjs` reads the **built** output. `dist/index.html` must be CSP-clean and
  reference only assets that exist; the chrome is rendered with `react-dom/server`'s `renderToStaticMarkup`
  from `dist-ssr/` and the ids/roles are asserted against that markup, per view. #431 made the role checks
  **stricter, not looser**: there are now exactly **two** tablists (the open files and the preview switch,
  each with one selection and one tab stop) and **no** page-level one, the tree must be `role="tree"` with a
  single selected row and a single tab stop, and the status-line footer must exist. A page that put the view
  switch back above the workspace would fail the "two tablists, one of them `Preview views`" check while
  every old id still resolved.
- `scripts/ci/preview_studio_smoke.mjs` drives the built `preferences`/`gallery` entries and the rendered
  chrome under a stub browser; the codec is still imported from source.
- `scripts/preview_budget_measure.py` walks the built `designer/web/dist/` as a **directory** instead of
  naming files, so an asset that ships is weighed by construction, and measures all three Wasm modules
  the studio loads (section 5.3).
- `.github/workflows/pages.yml` runs `npm ci` + `npm run build` in `designer/web` and assembles the site from
  `dist/` plus the three Wasm modules, the samples and the build id.
- `tests/golden/perf/preview_budget.json`: `download_gzip_bytes` re-recorded (section 6).
- **Browser verification** is the acceptance bar for a layout change, and it is developer-run (no lane has a
  browser): `scripts/ci/preview_browser_verify.mjs` (the merged harness, 14 checks),
  `scripts/ci/preview_browser_layout.mjs` (#431's layout measurements, minimally updated for the dock; W6
  #563 rewrites it) and `scripts/ci/workbench_dock.mjs` (W5's drags, keyboard docking and persistence),
  `scripts/ci/preview_browser_live_edit.mjs` (#444's six acceptance clauses: an edit applies with no click,
  the count survives it, a syntax error keeps the frame, Restart app resets it, a static scene draws zero
  frames, and a burst is one frame), `scripts/ci/preview_browser_author_css.mjs` (#390's acceptance: the
  built-in look on an empty sheet, author CSS replacing it, `var()`, and a bad rule reported while the frame
  still paints), and `scripts/ci/preview_browser_css_live_edit.mjs` (**the combined case, and the one neither
  feature's own probe covers**: with author CSS applied, a source edit must keep the author's styling -- a
  sheet dropped from `tessera_canvas_apply` repaints the built-in dark, so the colour is read after a
  keystroke, not after a button). All five are Playwright scripts run from a directory that resolves
  `playwright`; they are not repository dependencies.

## 6. The P6 budget, three times

`download_gzip_bytes` went 417327 -> 436794 (#392) -> 470568 (#422) -> 492748 (#387) -> 497988 (#431) ->
2378778 (#444's re-measurement of the enlarged tree). Every one is a reviewed edit of the budget file with
the reason in the diff, because `--update` refuses to loosen an entry (a re-pin on a loaded box would record
machine load as headroom).

### 6.1 #392 and #422

- **#392 (the merge):** the merged artifact ships *all three* views plus two redirect stubs in one artifact,
  where the old one was one page and one module (+16911 bytes of page, plus a re-measurement of the
  diagnostics module).
- **#422 (the conversion):** the framework floor is **measured, not estimated** -- a control `vite build` of
  `createRoot(...).render(<p/>)` from the same lockfile is 69007 gzip bytes of JS. The page's own JS is
  therefore 82797 - 69007 = 13790 bytes against 44001 bytes of hand-written modules before, and CSS plus
  HTML fall by 5022; net +33774. React's component model is cheaper than the `document.createElement` tree
  it replaces on this page.

### 6.2 #431 (the one-workspace layout): +5240 gzip bytes (+1.06%)

The two latency rows were left alone. Their values on this box moved with no C++ change at all, and the
ratchet's rule is that a warned wall-clock row is never re-pinned on whatever machine happened to measure it.
They still warn and still pass. (Section 5.3 is where they stopped warning: #395 gave them a number that
does not move with the box.) The `recorded` breakdown in the same entry was also left alone here, which
turned out to matter -- section 5.3 explains the 22180-byte gap that opened as a result.
Measured on the same box with the same zlib, decomposed with an **esbuild metafile** against a control build
of the same sources at `origin/main` 8efe707c (same lockfile, same Vite):

| Component | Δ gzip bytes |
|---|---|
| page JS 83822 -> 88017 | **+4195** |
| page CSS 2434 -> 3155 | **+721** |
| `index.html` 1212 -> 1210 | **-2** |
| the four public files, the font, both diagnostics module files | **0** |
| **net** | **+4852** (page), **+5240** (the enforced total, which also includes the unchanged module) |

The JavaScript delta is **this page, not the framework**: the `react`, `react-dom` and `scheduler` modules
total 221385 minified bytes in the base build and 221433 here (+48, from the one extra hook this page calls),
so the whole +4195 gzip is the workspace's own code (+13463 minified `src/` + `project-paths.js` bytes):

- **+2381 minified** is `project-paths.js` entering the shipped bundle for the first time. It was previously
  imported only by the Node gates. #431 is the first slice that needs the confinement rule *in the browser*,
  because the tree must not show a path the compiler would resolve differently.
- **+11082 minified** is the workspace itself: the project tree (+2362), the tabbed editor and its strip
  (+3156), the preview column and its view switch (+1763), the per-file buffer state (+2074), the project
  model (+1653), the session wiring (+2257, less App's own -1598), the status line's cells (+551), the
  sessions' `sampleSource`/`ProjectReader` signature changes either side and the small edits in
  `page-state` (+67) and `canvas-pane` (+213), **less** the code the move retired -- the top-level tab strip
  (-813 in `StudioChrome`) and the stand-alone editor pane (-872 in `ViewPanes`).

The CSS delta is the split by region (one `app.css` at 2.44 kB gzip -> six files at 3.15 kB together, 8.96 ->
13.24 kB raw; the split is required by the 500-line source limit, and Vite still emits **one** hashed CSS
file): a three-column grid, the tree's rows, the editor's tab strip and two breakpoint blocks. The result is
inside the 6 MB product budget (`designer-studio-v1.md` section 2.3) with 5.5 MB to spare.

### 6.3 #444 (live edit): +1030 gzip bytes (+0.04%)

Three numbers moved, all of them the change itself:

| Component | Î” gzip bytes | Why |
|---|---|---|
| page JS 88017 -> 88863 | **+846** | `page-live.ts` (the debounce controller) plus the apply path split out of `canvas-session.ts` |
| `index.html` 1210 -> 1211 | **+1** | the state row's two elements |
| `tessera_canvas.wasm` 1289503 -> 1289640 | **+137** | the one new export (`tessera_canvas_apply`) and its carry/state serializer |

The diagnostics and scene-preview modules, the font, the CSS and the four public files are byte-identical,
so the enforced total moved by exactly those three. The page delta is a *replacement*, not an addition: the
canvas session now calls two exports (`tessera_canvas_apply`, `tessera_canvas_draw`) where it called the one
stateless `tessera_canvas_frame`, and the old path's per-frame argument marshalling (`texture-view` handle
plus the source strings every frame) is what the new one removes. Nothing on the page is a new dependency:
the debounce is `setTimeout`, which the page already used.

The re-pin was a reviewed edit rather than `--update` (which refuses a raise), recorded from the
measurement with `--check --size-only`. `edit_compile_ms` is untouched by this slice -- the module it times
is byte-identical (SHA-256 `7A6FF0AD...095E7F`) and gained no compile work -- and its row fails on a box
whose calibration lands near 4.5 ms, which is a fact about the measuring host rather than about this change;
section 5 describes the cross-host history of that row.

## 7. What this does not do

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

Out of scope by #392, #422, #431, #390, #444 and #171 and named here so a reader does not infer them: a
router and user component instances (the scene builder does not render `<Home />`, so composing screens
needs an engine slice), sharing a whole project (waits for cloud storage), adding, deleting and renaming
files from the tree (the folder picker, the zip drop, Save and Download landed with #391), a real code
editor widget (the textarea is per file, which is what #431 asks for), text drawing (#387, shipped),
conditional children (#389), and #171's editable inspector panel (the picker overlay, skins, `source.patch`
and the engine's override layer, #536, shipped). The theme toolbar toggle is now applied (section 1.1); `assets.theme` still did not
arrive with #390, so the *project's* theme asset is carried-but-not-applied
([`compiler-wasm.md`](compiler-wasm.md) section 10.2).

#444 added **one engine export** (`tessera_canvas_apply`) and the state/`frames` fields of its report; #171's
skins slice added one more (`tessera_canvas_set_safe_area`) and the `insets`/`cutouts` fields of the
simulated metrics. Neither changed the evaluation, the carry rule, the paint path or the layout engine, which
already resolved a `SafeArea` from a `WindowMetrics`. #431 and #390 added **no engine feature to the page**:
#431 moved the page's chrome into the layout the issue draws and made the editor per file; the project
preview replaced #390's textarea with real project files that the entry imports, carried by each session's
`ProjectReader` snapshot. The Wasm builds stay three modules, the studio stays single-threaded (ADR-045
decision 5), and every page-to-engine call stays a message with transferable buffers.