# Design-time viewer (Wasm + WebGPU)

Status: design for Big Step D. `designer/web` and `designer/native` are placeholders. This is a
design-time bridge only; see [`bridges.md`](bridges.md) and ADR-024. Native inspector/hot reload:
[`devtools-protocol-v1.md`](../specs/devtools-protocol-v1.md).

The designer runs the **real C++ engine** in a browser via Emscripten and WebGPU/Dawn. It is
not an emulator and not a separate renderer. Production still compiles to native C++/Vulkan.

## 1. Goals

- Instant preview without a native build/emulator.
- Same UI/renderer code as native; only the GPU backend and platform layer differ.
- Hot reload of components/styles without losing state where safe.
- Deterministic reference frames for comparison with native output.
- No React runtime in production; React semantics exist only at design time.

## 2. Architecture

```text
Browser
  ├─ JS shell: canvas, DOM panels (tree/style/diagnostics), editor, file I/O (no user code)
  └─ Wasm module (static, built once; ADR-037)
       ├─ Tessera front end: lexer/parser/analyzer (same library as the native CLI)
       ├─ IR lowering + IR evaluator (hooks, expression subset, handlers)
       ├─ UI/style/layout/text engine + batched 2D renderer
       ├─ designer services: picker, selection, overlay, style override, patch writer
       └─ WebGPU RHI backend (Dawn/browser surface)
Native host (optional for the filesystem workflow)
  ├─ tessera CLI / codegen (same front end, native C++ output)
  ├─ file watcher + file writes
  └─ diagnostics/assets
```

The Wasm module uses the same `ui`, `rendering/gpu`, and `rhi` code. The WebGPU backend
implements the RHI; Vulkan is never compiled into Wasm (ADR-002). The module is never recompiled
per edit: user UI code is data (source text -> IR) executed by the evaluator, never by JavaScript.

### 2.1 Source to pixels

```text
source text (editor/host) -> front end (changed components only) -> IR
  -> IR diff vs last good IR -> evaluator patches scene in place
  -> style/layout/paint dirty -> one frame
```

- Parsing and rendering happen inside the Wasm engine. The time from an edit to the updated
  preview has no target yet; one must be set and measured (section 10). If parsing could block the
  browser main thread, parsing may run in a Web Worker that returns an immutable IR snapshot
  (undecided, section 10).
- The evaluator must match generated C++ semantics; unsupported constructs are rejected by the
  front end, never discovered at preview time. A differential suite compares the evaluator and
  generated native code on the same IR.

## 3. IPC

Messages are versioned JSON (a binary encoding is not designed yet). The normative message
definitions are in [`designer-protocol-v1.md`](../specs/designer-protocol-v1.md); the table
below is a summary:

| Direction | Message | Purpose |
|---|---|---|
| host -> viewer | `project.load` | config, component list, assets |
| host -> viewer | `pick.mode`, `selection.set`, `style.*` | picker and style-edit requests |
| host -> viewer | `source.update` | changed TSX/CSS text (viewer parses it itself) |
| host -> viewer | `asset.update` | font/image/theme change |
| viewer -> host | `ready`, `error`, `diagnostic` | status/logs |
| viewer -> host | `selection.changed` | picked component/node ids + source span |
| viewer -> host | `source.patch` | minimal text edit from a style commit (host/editor applies) |

The bridge is local-only by default; no remote code execution. Never expose arbitrary
filesystem/network access to the browser context.

## 4. Hot reload

```text
source change -> hash -> viewer front end (changed components) -> IR diff
  -> evaluator patches scene -> preserve state by component/key where safe
  -> request one frame
```

- Native C++ code generation is not part of the preview loop; it runs only when building the
  production app.

- Component identity + keys decide state preservation.
- Style-only changes do not rebuild the tree.
- Structural changes reconcile in place; removed nodes are retired.
- A failed parse/analysis keeps the last good IR per component and shows diagnostics at the
  error span; no white screen. The parser recovers at component granularity while typing.
- Full reload is the fallback when a patch cannot be applied safely.

## 5. Rendering and lifecycle

- WebGPU canvas sized to the preview viewport (device pixel ratio aware).
- Render-on-demand; request animation frames only when dirty/animating.
- Browser visibility/occlusion pauses rendering.
- Resize/DPR changes recreate the surface/swapchain; keep old resources until retired.
- No blocking GPU waits on the browser main thread; use queue callbacks/promises.
- WebGPU validation errors become diagnostics; never crash the page.

## 6. Determinism and parity

- Preview uses the same fixed clock/DPI/fonts for a "reference mode".
- Native goldens (lavapipe/software) are the source of truth; WebGPU frames are compared with
  tolerance.
- Shared shader manifest keeps GLSL/SPIR-V and WGSL bindings aligned (ADR-011).
- A parity test renders the same scene on Vulkan-headless and WebGPU-headless where possible.

## 7. Security

- The viewer is sandboxed by the browser; no native filesystem/network access.
- Project files are untrusted input: validate config/paths; reject absolute/traversal paths.
- No arbitrary JavaScript execution from components; the compiler controls the subset.
- CSP and local-only binding; do not expose the host bridge to the public network.
- Assets are decoded with size/format limits; no remote fonts in reference mode.

## 8. Developer experience

- Inspect node tree, computed style, layout boxes, draw calls, and dirty reasons.
- Picker and style editing: see 8.1 and 8.2.
- Show diagnostics with file/line/column; jump to editor.
- Toggle theme, DPI, viewport size, safe areas, fold posture, reduced motion.
- Record/replay input and metrics for deterministic repro.

### 8.1 Picker

- Every scene node carries `(componentId, instanceKey, nodeId, sourceSpan)` from the IR.
- Pick mode: hovering highlights the node under the pointer (its box, margin and padding areas,
  and its name); clicking selects it. The default selection is the deepest component instance
  under the pointer. Its primitive nodes (View, Text, and so on) and its ancestors are selected
  through the breadcrumb or the tree panel, or by `Alt` click (primitive) and `Ctrl`/`Cmd` click (parent
  instance) as in [`designer-protocol-v1.md`](../specs/designer-protocol-v1.md) section 8.1.
- Selection can also be set from the tree panel or from the editor cursor (`source -> node`).
- The overlay (highlight, handles, guides) is drawn on a designer-only layer: no semantics,
  no automation IDs, no effect on input routing while pick mode is off, excluded from goldens.
- Items of virtualized lists (see `docs/specs/virtualization-v1.md`) can be selected only while
  they exist in the scene. After a hot reload the selection is restored by matching
  `(componentId, instanceKey)`; if no node matches, the nearest ancestor that does match is selected.

### 8.2 Style editing

- The panel shows computed style, matched rules with specificity, cascade origin (inline, class
  rule, theme token, UA), and layout box values.
- A property edit applies to a designer override layer on top of the cascade: restyle only
  (no re-parse), validated by the stylesheet engine; unsupported properties/values report the
  same diagnostic as the compiler (compatibility matrix).
- Committing an edit chooses a destination: this instance's inline `style`, a class rule in the
  CSS file, or a theme token. The engine emits a minimal, span-preserving text patch
  (`source.patch`) that leaves unrelated text, formatting and comments unchanged; the host or
  editor applies it. The changed
  file then round-trips through the front end to the same IR and the override is cleared.
- Undo/redo and revert-all live in the designer; uncommitted overrides are visibly marked.
- The engine never writes files, edits only UI sources (TSX/CSS/tokens), and never touches
  developer C++ in protected regions (ADR-008). Mutation exists only in designer builds and only
  on the local machine (`docs/specs/devtools-protocol-v1.md`). Editing component props and
  children is not part of this design and has no scheduled release.

## 9. Testing

- Wasm build smoke test in CI (compile + load + render a frame).
- IPC protocol tests (versioning, malformed messages).
- Hot reload tests: style-only, structural, component rename, failed compile.
- Parity images: native vs WebGPU with tolerance.
- Browser matrix: Chrome/Edge/Firefox; WebGPU availability fallback message.
- Differential: evaluator vs generated native code on a shared corpus (scene snapshots + goldens).
- Picker: hit-test/granularity/selection restore after hot reload; overlay excluded from goldens.
- Style edit: override validation, patch round-trip yields identical IR, formatting/comment
  preservation, undo/redo, unsupported-property diagnostics.
- Parser: component-granular recovery on partial edits; malformed/huge input limits.

## 10. Open questions

- Wasm module size/startup budget. (The front end runs in a Web Worker and the designer requires a host in
  V1: [`designer-protocol-v1.md`](../specs/designer-protocol-v1.md) sections 11 and 14.)
- Patch format for hot reload (IR diff vs generated C++ diff).
- State preservation across structural changes (not in V1: [`designer-protocol-v1.md`](../specs/designer-protocol-v1.md) section 14).
- Dawn version and WebGPU feature floor.
- Multi-view/multi-window preview.
