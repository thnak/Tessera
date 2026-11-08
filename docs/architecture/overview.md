# Tessera architecture overview

Status: living document. [Big Step B1](../../AGENTS.md#big-steps-group-work-dont-micro-step) (renderer core) is implemented; B2 (runtime slice) and B3 (window/swapchain/OS) are documented but not implemented.

Tessera is a cross-platform C++ UI framework with a JSX/React design DSL, a build-time compiler
to native C++, a native Vulkan 1.3 renderer, and a WebAssembly + WebGPU design-time preview.
It is an **application framework**, not a game engine or a benchmark.

## 1. Design principles

1. **Native production, design-time DSL.** JSX/React semantics exist at design time; production
   is generated, readable C++ with no React runtime.
2. **Two backends, one RHI (render hardware interface).** Vulkan 1.3 native; WebGPU/Dawn for Wasm (WebAssembly). Renderer logic sits above
   the thin RHI ([ADR-001](../../decisions/ADR-001-vulkan-1-3-baseline.md) (Vulkan 1.3 baseline), [ADR-002](../../decisions/ADR-002-webgpu-dawn-for-wasm.md) (WebGPU/Dawn for WebAssembly), [ADR-003](../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin render hardware interface and batched 2D renderer)).
3. **Buy solved infrastructure.** Yoga, FreeType/HarfBuzz/SheenBidi/ICU, VMA (Vulkan Memory Allocator)/volk, and a vector
   path library ([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md) (vector path rasterization), [ADR-005](../../decisions/ADR-005-yoga-layout-adapter.md) (Yoga layout adapter), [ADR-006](../../decisions/ADR-006-text-stack.md) (text stack)). Build the compiler, [scene graph](../reference/glossary.md), runtime, and agent loop.
4. **Deterministic by construction.** Fixed clock/DPI/inputs/fonts; [goldens](../reference/glossary.md) from a software path
   ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests)), never a real GPU framebuffer.
5. **Power-aware.** Static UI idles; no busy-wait, no full-GPU target; [render-on-demand](../reference/glossary.md) and
   adaptive quality are defaults ([ADR-019](../../decisions/ADR-019-power-aware-runtime.md) (power-aware runtime defaults)).
6. **Never block the UI thread.** Async/await, bounded queues, state machines; no indefinite
   GPU waits or blocking I/O on the UI thread ([ADR-021](../../decisions/ADR-021-async-runtime.md) (async runtime)).
7. **OS compliance in the HAL (hardware/OS abstraction layer).** UI sees `WindowMetrics`, never window handles, OS messages, or
   swapchains ([ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md) (window resize, display scale, and lifecycle), [ADR-023](../../decisions/ADR-023-os-event-normalization-and-foldables.md) (OS event normalization and foldables)).
8. **Static linking and permissive licenses.** Single executable; system GPU/OS components stay
   dynamic ([ADR-012](../../decisions/ADR-012-static-linking.md) (static linking), [ADR-013](../../decisions/ADR-013-project-license-mit.md) (MIT license)).
9. **Structured diagnostics.** Logs/traces/metrics/crash/capture are bounded, optional, and
   privacy-safe ([ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling)).
10. **Small, explicit [V1](../reference/glossary.md) surface.** Unsupported React/CSS/patterns are rejected loudly rather
    than half-implemented.

## 2. System map

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Toolchain (build time)                                               │
│  tessera CLI ─ tesserac ─ JSX/TSX parser ─ analyzer/rules ─ IR       │
│       └─ codegen ─ clang-format ─ protected-region merger ─ manifest │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ generated C++ + bindings.json
┌───────────────────────────────▼──────────────────────────────────────┐
│ Application runtime (native)                                         │
│  App/runtime loop ─ HAL (window/display/input/power/timer)            │
│       ├─ scene graph + input dispatch + focus                        │
│       ├─ style engine ─ layout (Yoga) ─ animation ─ text             │
│       ├─ renderer (batched 2D) ─ RHI ─ Vulkan 1.3                    │
│       ├─ services (i18n/navigation/storage/networking/notifications) │
│       └─ diagnostics (logging/tracing/metrics/crash/overlay)          │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ same renderer/UI code
┌───────────────────────────────▼──────────────────────────────────────┐
│ Design-time viewer (Wasm)                                            │
│  Emscripten C++ ─ WebGPU/Dawn ─ live view (IR) ─ IPC/hot reload│
└──────────────────────────────────────────────────────────────────────┘
```

The compiler is **not** linked into production apps. The runtime never parses JSX/TSX or JavaScript, and never parses CSS
source at runtime unless explicitly configured for development.

## 3. Build-time pipeline

```text
tessera.json
  -> validate schema (strict; unknown fields are errors)
  -> discover ui.sourceDirs / include globs
  -> parse JSX/TSX with the Tessera front end (ADR-036)
  -> Tier 1: React rules (hooks, purity, static structure)
  -> Tier 2: bindings (`bind::<id>`) and node lowering
  -> IR (`docs/specs/ir-v1.md`)
  -> codegen: one `<Component>.g.h/.g.cpp` per component
  -> clang-format (pinned)
  -> protected-region merge (ADR-008)
  -> bindings manifest + generation report
```

The front end named in the second step is the Tessera-owned JSX/TSX parser decided in [ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md) (own JSX front end and React-like model).

Hashes of source, relevant config, generator version, and formatter config decide whether a
component is regenerated. Generated output is stable: no timestamps, absolute paths, or random
identifiers. See [`compiler.md`](../design/compiler.md) and
[`project-generator.md`](../guides/project-generator.md).

## 4. Runtime pipeline

One tick (a fixed-timestep simulation step) and the frame it may produce:

```text
OS/HAL events -> normalized queue
  -> input dispatch (focus, capture, hover/active)
  -> style resolution (cascade, custom properties, pseudo-states)
  -> layout (Yoga + measure callbacks)
  -> animation sampling (fixed timestep)
  -> paint/draw-list build (dirty domains, caches)
  -> renderer batching -> RHI command list -> Vulkan
  -> present/pacing (on-demand, FPS cap, vsync/VRR)
  -> diagnostics/metrics
```

Not every stage runs every tick. [Dirty domains](../reference/glossary.md) and render-on-demand mean a static screen can
produce **zero presents and zero GPU submissions** after settling. See
[`game-loop.md`](../design/game-loop.md), [`dirty-rendering.md`](../design/dirty-rendering.md),
and [`performance.md`](../design/performance.md).

## 5. Layer contracts

| Layer | May depend on | Must not depend on |
|---|---|---|
| `core` | C++ standard library | OS, GPU, UI, compiler |
| `hal` | `core` | UI, renderer backends |
| `platform/*` | `hal`, `core`, system APIs | UI, renderer backends |
| `ui` | `core`, `hal`, RHI *types* | Vulkan/WebGPU/Win32/X11 headers |
| `rendering` | `core`, RHI, UI display lists | OS window handles in renderer logic |
| `runtime` | all runtime modules | compiler/parser |
| `compiler` | build-time libs | runtime renderer/UI |
| `designer` | runtime, Wasm/WebGPU | native-only platform code |
| `services` | `core`, `hal` | UI internals |
| `diagnostics` | `core` (cross-cutting) | UI/renderer internals |

OS-specific code lives under `src/platform/{windows,linux,macos,android}` and implements
`src/hal/` interfaces. The UI layer sees normalized metrics/events, not platform types.

**No production bridge in the UI/render/input/memory hot path.** Bridges are limited to OS
edges, build-time tooling, and the design-time viewer; UI objects never cross JNI (Java Native Interface)/ObjC/Wasm
boundaries ([ADR-024](../../decisions/ADR-024-native-first-boundaries.md) (native-first boundaries), [`bridges.md`](../design/bridges.md)).

## 6. Threading model

- **UI thread** owns the scene graph, style/layout/animation state, input dispatch, and
  command recording. No blocking waits.
- **Worker pool** handles IO, decoding, compilation/preprocessing, and CPU-heavy work; results
  return through a bounded completion queue drained on the UI thread.
- **GPU submission** is explicit; [frame pacing](../reference/glossary.md) uses fences/timeline semaphores and never
  `vkDeviceWaitIdle` on the UI thread.
- **Render thread** is optional in B3; if introduced, it consumes immutable draw lists and
  owns presentation. B1 is synchronous and offscreen.
- **No detached tasks.** Every task has an owner and a cancellation path ([ADR-021](../../decisions/ADR-021-async-runtime.md)).

See [`threading-and-memory.md`](../design/threading-and-memory.md) and
[`async.md`](../design/async.md).

## 7. Determinism

Golden/headless mode:

- fixed simulation clock and delta
- fixed offscreen size, DPI (dots per inch, the display scale)/scale, and refresh behavior
- committed fonts, no system fonts, no subpixel positioning
- synthetic input/OS event scripts
- software/lavapipe rendering as the source of truth
- no wall clock, network, or random IDs in generated output

A golden failure produces an image diff plus a structural report (node ids, bounds, colors,
dirty reasons). See [`testing.md`](../design/testing.md) and
[`golden-tests-v1.md`](../specs/golden-tests-v1.md).

## 8. Configuration

- Project: `tessera.json` — [`project-config-v1.md`](../specs/project-config-v1.md)
- Runtime: loop/FPS/vsync/VRR/dirty/power/async/resize —
  [`runtime-config-v1.md`](../specs/runtime-config-v1.md)
- Buffers/user tunables: engine buffer defaults and the explicit user-adjustable allowlist —
  [`buffer-config-v1.md`](../specs/buffer-config-v1.md)
- Diagnostics: logging/tracing/profiling/crash/overlay —
  [`diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md)
- C++ API mirrors the schema (`tessera::AppConfig`).

Unknown fields are errors. CLI (command-line interface) flags override for one invocation and never rewrite the file.

## 9. Status and reading order

Implemented B1:

- thin RHI + [null backend](../reference/glossary.md)
- Vulkan 1.3 offscreen backend + batched 2D
- scene graph/hit-testing/events
- merger prototype

Documented, not implemented: stylesheet engine, Yoga/text, input, fixed-step loop, dirty
rendering, frame pacing/VRR, async runtime, diagnostics, window/swapchain, OS events/foldables,
compiler/IR/codegen, Wasm designer, services, accessibility, packaging.

Start with [`docs/README.md`](../README.md), then
[`module-map.md`](module-map.md), [`hal.md`](../design/hal.md),
[`ui-model.md`](../design/ui-model.md), and [`compiler.md`](../design/compiler.md).

## 10. Non-goals (V1)

- General-purpose 3D/PBR engine, scene lighting, or physics.
- Browser engine, DOM, or a React runtime in production.
- Full CSS module coverage (full syntax + a documented semantic matrix instead).
- Dynamic plugin ABI (application binary interface) or hot-loadable native modules.
- macOS in V1 (deferred, [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md) (macOS via MoltenVK, deferred from the first release)); Android/mobile integration is listed in [Big Step F](../../AGENTS.md#big-steps-group-work-dont-micro-step) (production breadth), after Windows and Linux.
- Bit-identical golden images from real GPUs.
