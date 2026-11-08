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
  ├─ Wasm module (C++ engine + UI + renderer)
  ├─ WebGPU (Dawn/browser) surface
  ├─ custom React reconciler (design-time component semantics)
  ├─ JSX/TSX loader + hot-reload client
  └─ IPC bridge <-> native host
Native host
  ├─ tessera CLI / compiler
  ├─ file watcher
  ├─ incremental generator
  └─ diagnostics/assets
```

The Wasm module uses the same `ui`, `rendering/gpu`, and `rhi` code. The WebGPU backend
implements the RHI; Vulkan is never compiled into Wasm (ADR-002).

## 3. IPC

Messages are versioned JSON (or a compact binary encoding later):

| Direction | Message | Purpose |
|---|---|---|
| host -> viewer | `project.load` | config, component list, assets |
| host -> viewer | `component.update` | changed TSX/CSS + compiled IR/code |
| host -> viewer | `asset.update` | font/image/theme change |
| viewer -> host | `ready`, `error`, `diagnostic` | status/logs |
| viewer -> host | `component.inspect` | select node, show style/layout |
| viewer -> host | `state.patch` | optional state restore after reload |

The bridge is local-only by default; no remote code execution. Never expose arbitrary
filesystem/network access to the browser context.

## 4. Hot reload

```text
file change -> hash -> incremental compile -> IR/code patch
  -> viewer applies patch -> reconcile scene
  -> preserve state by component/key where safe
  -> request one frame
```

- Component identity + keys decide state preservation.
- Style-only changes do not rebuild the tree.
- Structural changes reconcile in place; removed nodes are retired.
- A failed compile keeps the last good frame and shows diagnostics; no white screen.
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
- Click-to-select maps preview nodes to source spans.
- Show diagnostics with file/line/column; jump to editor.
- Toggle theme, DPI, viewport size, safe areas, fold posture, reduced motion.
- Record/replay input and metrics for deterministic repro.

## 9. Testing

- Wasm build smoke test in CI (compile + load + render a frame).
- IPC protocol tests (versioning, malformed messages).
- Hot reload tests: style-only, structural, component rename, failed compile.
- Parity images: native vs WebGPU with tolerance.
- Browser matrix: Chrome/Edge/Firefox; WebGPU availability fallback message.

## 10. Open questions

- Whether the design-time reconciler uses a JS runtime or a compiled interpreter.
- Patch format for hot reload (IR diff vs generated C++ diff).
- State preservation across structural changes.
- Dawn version and WebGPU feature floor.
- Multi-view/multi-window preview.
