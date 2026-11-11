# Designer studio v1: browser-hosted live view

Status: proposed (Big Step D). Decision: [ADR-045](../../decisions/ADR-045-browser-hosted-studio.md).
Design note: [designer.md](../design/designer.md) (architecture, hot reload, picker).
Protocol: [designer-protocol-v1.md](designer-protocol-v1.md).
Chrome preview: [window-chrome-v1.md](window-chrome-v1.md).

The studio is the "open a URL and work" form of the designer: the real C++ engine compiled to Wasm
runs in the page, the page hosts the project (folder, zip or sample) and the code editor, and the
left pane shows the engine output inside a simulated device frame. No native toolchain, no build
step in the loop, nothing uploaded to a server.

## 1. Purpose and non-goals

In scope: a static site hosted from this repository; two-pane workspace (preview + code/file tree);
project import and export; live view; device skins and simulation controls; a Node stdio mode of the
same engine for coding agents (MCP).

Not in scope: the production app (V1 compiles to native C++/Vulkan), the native inspector
([devtools-protocol-v1.md](devtools-protocol-v1.md)), a hosted compile service or any server-side
code, multi-user collaboration, OS emulation (the engine is real; the device frame is only a
simulation), editing C++ or protected regions ([ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md)),
and using Tessera itself to render the studio's own chrome.

## 2. Deployment model

### 2.1 Hosting

- The site is built from this repository and deployed to GitHub Pages at
  `https://<org>.github.io/Tessera/designer/` (Tessera's canonical remote is `thnak/Tessera`). This
  is a static HTTPS origin with no server code, no cookies and no auth. `git.io` was a GitHub URL
  shortener retired in 2022 and is not used.
- The build is an artifact, never committed: Emscripten compiles the engine, the site assembles
  `index.html`, the JS host shell, the viewer worker bootstrap, the Wasm module, skins and samples,
  and `actions/deploy-pages` publishes it. Deployment runs only from `main`; a failed studio smoke
  test fails the job and blocks deployment.
- Versioned paths: immutable hashed assets under `/designer/v<major>/`, `index.html` served with
  revalidation, and a `/designer/latest/` alias. A running page never mixes two engine build ids;
  the status line shows the id.
- Local development uses the same static tree behind any HTTP server. There is no SSR path.

### 2.2 Platform constraints

- **No custom response headers on GitHub Pages.** `Cross-Origin-Opener-Policy` and
  `Cross-Origin-Embedder-Policy` cannot be set, so `SharedArrayBuffer` is unavailable and the Wasm
  build is **single-threaded** (`-sPTHREADS=0`). Off-main-thread work uses Web Workers and
  transferable buffers, not shared memory. If threads become necessary, the design must either add
  a service-worker cross-origin-isolation shim (a known hack with reload semantics) or move to a
  header-capable static host; this is a recorded constraint, not a silent fallback.
- **WebGPU floor.** The preview needs WebGPU (Chrome/Edge 113+, Safari 26+, Firefox 141+ on
  supported platforms). A browser without it gets a static "not supported" page listing the required
  versions and the local-run instructions; there is no CPU-preview fallback in V1.
- **No preview deployments.** GitHub Pages serves one site per repository, so pull-request previews
  are not part of V1 (the wasm smoke test is the gate instead).
- **Repository size.** GitHub Pages has a 1 GB published-site soft limit; builds are deploy
  artifacts, not repository content, and the site does not bundle fonts or ICU data (see 2.3).
- Requests for large assets use the normal GitHub Pages CDN; no range or gzip negotiation is
  controlled by the project.

### 2.3 Size and startup budget (provisional)

| Item | Target | Note |
|---|---|---|
| Initial compressed download | ≤ 6 MB | engine + front end + evaluator + shaders + skins; no fonts, no ICU data |
| Cold time-to-interactive | ≤ 4 s | 2020-class laptop, warm CDN |
| Project load (sample) | ≤ 1 s | parse + first frame |

The engine uses the builtin break-rule backend instead of the ICU reduced-data blob, and project
fonts load from the project, not the bundle. The budget is measured by the studio smoke test and
reported as a warning; the owner decided (2026-11-11) that it stays a warning in V1 and becomes a
hard gate only after D-S5 is stable.

## 3. Page composition

```text
+------------------------------------------------------------------------------+
| Tessera Studio   project  [device v] [orientation v] [theme v] [scale v]  Run |  studio chrome (HTML)
+--------------------------------------------------------+---------------------+
|  +--------------------------------------------------+  | Files    [search]   |
|  |  [menu bar / title bar - simulated system chrome]|  |  ui/                |
|  |  +--------------------------------------------+  |  |   App.tsx           |
|  |  |                                            |  |  |   Home.tsx          |
|  |  |        engine canvas (WebGPU, offscreen)   |  |  | styles/theme.css    |
|  |  |        app UI + app chrome drawn here      |  |  | tessera.json        |
|  |  |                                            |  |  +--------------------+
|  |  +--------------------------------------------+  |  | Editor   App.tsx    |
|  |  [status/navigation bar - simulated]             |  |  ...                |
|  +--------------------------------------------------+  | Diagnostics (2)     |
|  device skin: bezel, cutout, frame (HTML/SVG)          |                     |
+--------------------------------------------------------+---------------------+
| engine v0.x | WebGPU | chrome: rendered | diagnostics 0 | frame 2.1 ms        |
+------------------------------------------------------------------------------+
```

Four regions, all HTML/CSS; the engine draws only the canvas:

1. **Studio chrome** — toolbar, device/theme/scale selectors, run/reload, project actions.
2. **Preview well** — the skin frame plus the canvas.
3. **Workspace pane** — file tree, editor, diagnostics, selection/breadcrumb.
4. **Status line** — engine id, backend, simulation state, diagnostics count, frame time.

The page must not be rendered by Tessera in V1. Reasons: the engine being previewed must not be able
to break its own editor; native text editing, file access, clipboard and accessibility are cheaper in
the browser; and the shell can show a diagnostic when the engine fails to boot, which a
self-hosted editor could not.

### 3.1 Chrome layers

The word "chrome" means several different things in the studio and the naming is normative:

| Layer | Owner | Drawn by | In the studio |
|---|---|---|---|
| Studio chrome | studio | HTML/CSS | toolbar, panels, tabs |
| Simulated system chrome | OS in production, studio in preview | HTML/SVG skin | title bar, menu bar, status bar, navigation bar, cutout, in `system` mode only |
| Framework chrome | framework | the engine | `WindowChrome` title bar and window controls in `custom` mode; `AppBar` |
| App chrome | the app | the engine | toolbars, bottom navigation, in-page headers |

The last two are produced by the engine. The first is never part of the preview. The simulated system
chrome is a simulation whose geometry and colours must match production
([window-chrome-v1.md](window-chrome-v1.md)); it is never an input to the previewed app's layout
except through the insets it supplies. In `custom` mode the framework title bar is engine content, so
the studio shows it from the engine and hides the simulated one.

## 4. Host and viewer in one page

The studio reuses the [designer protocol](designer-protocol-v1.md) instead of defining a second one.

- The **page** is the host: it owns project files (in memory, plus File System Access API or OPFS),
  assigns file versions, applies patches, and shows diagnostics. It never runs user code.
- The **viewer** is the Wasm engine in a dedicated worker; it never reads or writes files.
- The transport is `postMessage` (protocol section 2.1) between the page and the worker, with the
  same envelope, limits and ordering rules as the WebSocket transport used by the native CLI.
- The viewer runs in a worker with the canvas transferred as an `OffscreenCanvas` where the browser
  supports WebGPU in workers. Where it does not, the viewer runs on the main thread with a visible
  "degraded" badge in the status line and a diagnostic; both paths keep the same engine build.
- Assets: the viewer normally fetches `/assets/<path>` from the host; in the studio the page hands
  each asset a URL (opaque `blob:` URL from OPFS or the imported zip) in `asset.update`, because
  there is no HTTP host serving the project.
- The native CLI host remains for filesystem work and codegen; a project can move between the two
  because the protocol and the file versions are the same.

The session starts with `hello` → `ready` → `project.load`, as specified; the studio adds no
message types. `theme.applied` (protocol section 6) is what the page uses to tint the simulated
system chrome from the engine's resolved record.

## 5. Simulated system chrome and skins

### 5.1 Geometry contract

The engine always receives the same geometry production would give it:

- **Desktop category, `system` mode** (`window.titleBar: "system"`): the canvas is the client area.
  The skin draws the title bar (and macOS menu bar) as a strip outside the canvas, exactly as the OS
  does; the viewer receives zero insets ([window-chrome-v1.md](window-chrome-v1.md) section 6).
- **Desktop category, `custom` mode** (`window.titleBar: "custom"`): the canvas is still the client
  area, but the engine draws the framework `WindowChrome` at its top; the skin draws only the outer
  bezel and adds no title strip. `viewport.set` height includes the bar, because the bar lives inside
  the client area; the studio switches modes live when the config changes.
- **Mobile category**: the canvas is the full display; the skin overlays the status bar, navigation
  bar and cutout on top of the canvas, and its insets are passed to the viewer as `safeAreas` in
  `viewport.set`, exactly as `WindowMetrics::insets` would arrive from the HAL.

A skin therefore declares the outer device box and the insets; the page derives the viewer size. The
skin must never change the engine's logical size behind its back: the size in `viewport.set` is the
size the preview renders at.

### 5.2 Skin manifest

Skins are data plus static art, never code. One directory per skin under the site's `skins/`:

```json
{
  "schemaVersion": 1,
  "name": "Generic phone",
  "category": "mobile",
  "os": "android",
  "size": { "width": 412, "height": 915 },
  "bezel": { "top": 12, "right": 8, "bottom": 12, "left": 8, "radius": 28 },
  "cutout": { "type": "punch-hole", "x": 206, "y": 14, "radius": 5 },
  "chrome": { "statusBar": 24, "navigationBar": 24, "gestureBar": true },
  "insets": { "top": 24, "right": 0, "bottom": 24, "left": 0 },
  "art": "frame.svg"
}
```

| Field | Required | Meaning |
|---|---|---|
| `category` | yes | `desktop`, `mobile`, `tablet` or `foldable`; selects the geometry mode of 5.1 |
| `os` | yes | `windows`, `macos`, `linux`, `android` or `ios`; selects the chrome variant and a preset |
| `size` | yes | Outer logical size before the preview scale |
| `bezel` | no | Physical frame drawn around the screen; defaults to zero |
| `cutout` | no | `none`, `notch` or `punch-hole` plus placement; drawn by the skin and included in `insets` |
| `chrome` | no | Title/menu/status/navigation bar sizes; defaults by `os` |
| `insets` | yes | `safeAreas` handed to the viewer; must contain the cutout and bars for mobile |
| `art` | no | SVG frame asset; the page draws a generated frame when absent |

- Chrome **colours** are not part of the skin: the page tints the simulated bars from
  `theme.applied` (`{background, foreground, scheme}`), so a theme switch retints the frame and the
  canvas in the same frame (window-chrome-v1 section 3.3). A skin that hard-codes a colour is a bug.
- Unknown fields are ignored; a missing required field or an invalid geometry (inset larger than the
  screen, negative size) rejects the skin with `TESSERA_STUDIO_SKIN` and it is not offered.
- Bundled V1 skins: generic desktop window (Windows 11, macOS, Linux), generic phone (Android, iOS),
  generic tablet, generic foldable (book and tabletop postures). All art is original or
  permissively licensed; vendor device art (Apple, Google, Samsung) is not redistributed, and
  user-provided skins are not part of V1 (owner decision 2026-11-11: bundled skins only).

### 5.3 Presets and controls

A preset is a named combination of skin, orientation, preview scale, theme and colour scheme,
localized in `presets.json`; the toolbar selects presets, not raw pixel sizes. A custom size entry
exists for width/height and DPR, which creates an ad-hoc skin from the generic frame.

## 6. Simulation controls

V1 controls and their protocol mapping:

| Control | Maps to | Effect |
|---|---|---|
| Device preset / custom size | skin + `viewport.set` size and `dpr` | Canvas and insets |
| Orientation | skin rotation + swapped size | Layout and media queries |
| Preview scale (50/75/100/150/200%) | page zoom of the skin, not `dpr` | Keeps the engine at device density |
| Theme and colour scheme | `viewport.set` `theme`, `colorScheme`; retint via `theme.applied` | Style cascade and simulated chrome |
| Reduced motion | `viewport.set` `reducedMotion` | Animation policy |
| Locale and direction | `viewport.set` `locale`, `direction` | Text, RTL |
| Text scale | `viewport.set` `textScale` | Font scale |
| Safe-area and cutout overlay | page-only overlay | Debug aid; excluded from screenshots |
| Fold posture (foldables) | `viewport.set` `foldPosture` | Two-pane layout |
| Freeze time | fixed clock in the viewer | Deterministic screenshots and animations |
| Input injection (hover, press, click, type, key, scroll, touch) | viewer input path, same as the real runtime | Drives hooks and events |
| Startup phase (P0/P2/P4) | viewer startup pipeline with a frozen stage | Loading screen, framework chrome and P2->P4 continuity (D-S9) |
| Screenshot | page canvas readback (WebGPU `copyTextureToBuffer` + PNG) | Artifacts for review |
| Pick mode | `pick.mode` | Selection from the preview |

Deferred: multi-window split, picture-in-picture, network throttling/offline, memory-pressure and
thermal simulation, and animation timeline scrubbing.

## 7. Project load, edit and save

- **Sources**: a bundled sample project; "Open folder" through the File System Access API (Chromium);
  drag-and-drop of a zip (all browsers); a directory previously imported into OPFS.
- **Accepted files**: `tessera.json`, `.tsx`/`.jsx`, `.css`, theme JSON, and the fonts/images listed
  in `tessera.json`. Other files are shown read-only with a lock badge and are never sent to the
  viewer. C++ and generated files are read-only; protected regions are never edited
  ([ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md)).
- **Validation**: `tessera.json` and assets are validated by the same in-tree validator compiled to
  Wasm (`tessera_schema`); an invalid config shows a blocking panel with the JSON path instead of a
  blank preview.
- **Edit loop**: edit → ~150 ms debounce → `source.update` (full text + a new version) → viewer parses
  and evaluates → diagnostics inline at the source span → one frame. The page never sends a partial
  file; the viewer's version rules reject stale text.
- **Save**: with an open folder, writes go back to disk on explicit save; without one, "Download
  project" exports a zip that contains every edit (patches already applied). Dirty state is always
  visible and a tab close warns; nothing is silently lost and nothing is uploaded.
- **Limits**: the per-file and per-project limits of [jsx-subset-v1.md](jsx-subset-v1.md) section 15
  apply (1 MiB per file, depth 128, 100 diagnostics, 64 components), plus a 32 MiB workspace cap with
  a clear error. Project text and assets are untrusted input; size checks run before parsing.
- **Samples** live in `samples/`; the studio loads them read-only and offers "Copy to workspace" so a
  visitor cannot edit the bundled copy.

## 8. MCP server mode (stdio)

The same engine has a Node entry point so a coding agent can validate and inspect a project without a
browser or a native build: `tessera-mcp`, speaking the Model Context Protocol over stdio (JSON-RPC
2.0). It is a **thin adapter**: every tool maps to one command of
[headless-automation-v1.md](headless-automation-v1.md) or one message of the designer protocol, so
there is no third protocol to keep in sync.

| Tool | Maps to | Purpose |
|---|---|---|
| `open_project` | `project.load` + `project.state` | Load a project directory (local path) |
| `list_files` | host file tree | Files, kinds, versions |
| `diagnostics` | `diagnostic` events | Compile errors with file/line/column |
| `get_ir` | `project.state` + IR access | The lowered IR for a component (early mode) |
| `apply_edits` | `source.patch` TextEdits | Apply edits in memory by default; `write: true` writes them to the project directory |
| `set_viewport` | `viewport.set` | Device size, DPR, theme, locale, safe areas, fold |
| `snapshot` | harness `snapshot` | Node tree, bounds, roles, text, focus |
| `query_style` | `style.inspect` | Computed style, matched rules, layout box |
| `pick` | `selection.set` by point | Node at a coordinate |
| `screenshot` | native headless renderer (Dawn/Vulkan, GPU) | PNG of the scene; best-effort visual, never a golden |
| `run_script` | harness script (`launch`/`click`/`assert`/...) | End-to-end checks with a report |

- **Early milestone**: compile-only mode. Before the evaluator and layout exist, the server supports
  `open_project`, `list_files`, `diagnostics` and `get_ir`; an agent can validate TSX/CSS against the
  real front end while the app is still being built. This is the first shippable MCP slice.
- **Determinism**: fixed clock and DPI, bundled OFL test fonts unless the project supplies fonts, no
  network, and canonical JSON for the structural and tool outputs. `screenshot` runs the native
  headless renderer (Dawn or Vulkan) because the Wasm module has no GPU context in Node; its pixels
  are best-effort visual evidence, never goldens, and CI skips GPU screenshots unless a device is
  provisioned ([golden-tests-v1.md](golden-tests-v1.md)).
- **Security**: stdio only (no listener), local paths only, no shell execution, project files are
  untrusted, and the server never edits C++ or files outside the project.

## 9. Hosting workflow

1. CI builds the Wasm module with the pinned Emscripten version
   ([dependencies.md](dependencies.md)) and runs the headless studio smoke test: instantiate, load a
   sample, parse, layout, render on the null backend, compare a structural digest.
2. CI assembles the site artifact (never committed), then `actions/deploy-pages` publishes it from
   `main`.
3. The status line and the page's `engine` field show the same build id as the deployed artifact, so
   a stale cache is visible.
4. A visitor can download the sample project and continue in the native workflow; nothing is lost by
   leaving the studio.

## 10. Security and privacy

- Project files never leave the browser: File System Access, OPFS and zip are local; there is no
  upload endpoint, no analytics, no telemetry, and no cross-origin request except the site's own
  assets.
- The page uses a strict CSP (no inline script, no remote script), bundled editor libraries with
  pinned versions and hashed asset names.
- User code never becomes JavaScript: the compiler subset is enforced by the front end, and the
  engine runs as Wasm in a worker.
- The MCP server is local-only over stdio and redacts absolute paths in diagnostics, as the other
  diagnostics paths do ([security.md](../design/security.md)).
- Importing a zip validates paths (no absolute paths, no `..`, no symlinks), file sizes and the
  allowed file set before anything reaches the engine.

## 11. Tests

- Studio smoke (CI): page loads, Wasm instantiates, a sample yields a frame on the null backend, no
  console errors, no network request outside the origin.
- Skins: manifest validation, defaults, insets → `safeAreas`, desktop vs mobile geometry, theme
  change retints without reload, invalid skin rejected with `TESSERA_STUDIO_SKIN`.
- Host loop: edit → diagnostic → frame, monotonic versions, save, download-zip round trip, dirty
  state, File System Access fallback path.
- Chrome preview: simulated bars use the `theme.applied` record; desktop `system` mode excludes the
  title bar from the canvas; `custom` mode draws the framework bar in the engine and hides the
  simulated one, and the content area starts below it; mobile canvas includes the bars as overlays
  and the viewer gets matching insets.
- Startup preview: freeze at P2 shows the framework chrome plus the loading subtree, freeze at P4
  shows the app UI; no frame is frameless in `custom` mode.
- Security: oversized file, zip-slip and absolute paths, non-UI file open, CSP, no cross-origin
  request.
- Budgets: compressed download and TTI recorded per build; a regression warns (owner decision: not a
  gate in V1).
- MCP: tool JSON schemas, error shape, determinism, cancellation, and the compile-only milestone on
  a corpus of valid/invalid TSX. Screenshot runs only on the GPU lane and is skipped otherwise.
- Startup phase freeze: stage ids match the startup spec; the same snapshot is produced twice from
  the same input.
- Browser matrix: current Chrome/Edge/Firefox/Safari; the unsupported page for no WebGPU.

## 12. Slices

| Slice | Work | Acceptance |
|---|---|---|
| D-S1 | This spec, ADR-045, protocol deltas, docs index | docs gate clean |
| D-S2 | Studio shell and host loop: file tree, editor, `postMessage` host, sample project, diagnostics, status line | sample edits produce a frame; smoke test |
| D-S3 | Skins and simulated system chrome; `theme.applied`; safe-area overlay | geometry and retint tests |
| D-S4 | Project IO: folder access, zip import/export, OPFS, download | round-trip and security tests |
| D-S5 | Wasm CI build and GitHub Pages deployment; smoke gate; budgets | deployed URL serves the current build id |
| D-S6 | MCP compile-only server (`open_project`, `diagnostics`, `get_ir`) | agent can validate TSX from stdio |
| D-S7 | MCP snapshot/style/pick/screenshot; harness script tool | deterministic results on the corpus |
| D-S8 | Remaining simulation controls (multi-window, PiP, animation timeline) | as decided |
| D-S9 | Startup-phase preview: freeze at P0/P2/P4, render framework chrome and the loading subtree, startup diagnostics in the panel | stage freeze + loading-screen golden; needs startup S3/S5 and D-S3 |

Prerequisites: D-S2 to D-S9 need the Wasm engine build, the WebGPU backend and the IR evaluator
(Big Step D items that do not exist yet: `designer/web` and `rendering/backend/webgpu` are
placeholders). D-S5 can land before D-S2 as a "diagnostics-only" page: the front end is enough to
validate TSX in the browser, which gives the hosted URL value early. D-S9 additionally needs the
startup pipeline of [startup-api-v1.md](startup-api-v1.md) slices S3 and S5 compiled to Wasm, so it
can only follow the startup work; it is planned, not parallel.

## 13. Decisions and open questions

Owner decisions recorded 2026-11-11: budgets are CI warnings, not gates, in V1; `OffscreenCanvas` +
WebGPU in a worker with a visible main-thread fallback; bundled skins only; MCP screenshots come
from the native headless renderer (GPU), not the Wasm module; `apply_edits` defaults to in-memory
with an explicit `write: true`; the studio previews startup phases P0/P2/P4 in V1; `viewport.set`
gains `locale`, `direction` and `textScale`; GitHub Pages is the only host in V1; native parity stays
a CI concern.

Open questions (no decision yet):

1. Concrete budget numbers and the first-paint target for the designer page; when the warnings turn
   into gates.
2. Which browsers must run the fallback path, and whether the fallback may use the CPU rasterizer
   instead of WebGPU.
3. The bundled skin and preset list: how many devices, which postures, and how much art ships.
4. GPU screenshot requirements for MCP: Dawn bindings in Node versus spawning the native `tessera`
   headless renderer, and how CI behaves without a GPU.
5. Startup preview fidelity: which stages are pinnable, whether it runs the real `AppHost` or a
   reduced stage machine, and how fixtures pin a frozen stage.
6. MCP tool naming, published JSON Schema, and versioning policy for the tool surface.
