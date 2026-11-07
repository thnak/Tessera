# Tessera documentation map

Start with [`../AGENTS.md`](../AGENTS.md) for the operating rules and [`../README.md`](../README.md)
for the project overview. This page is the map and reading order for the framework.

## 1. Architecture

- [`architecture/overview.md`](architecture/overview.md) — layers, pipelines, threading,
  determinism, status.
- [`architecture/module-map.md`](architecture/module-map.md) — source tree, build targets,
  ownership, dependency direction.

## 2. Decisions

[`../decisions/README.md`](../decisions/README.md) is the ADR index. Runtime-related ADRs:

- [ADR-005](../decisions/ADR-005-yoga-layout-adapter.md) — Yoga Flexbox + small Grid subset
- [ADR-006](../decisions/ADR-006-text-stack.md) — FreeType/HarfBuzz/SheenBidi/ICU
- [ADR-007](../decisions/ADR-007-compiler-ir.md) — neutral IR before codegen
- [ADR-008](../decisions/ADR-008-protected-regions-and-manifest.md) — protected regions/merge
- [ADR-009](../decisions/ADR-009-deterministic-headless-golden-tests.md) — deterministic goldens
- [ADR-016](../decisions/ADR-016-stylesheet-engine.md) — stylesheet engine
- [ADR-017](../decisions/ADR-017-input-and-fixed-step-runtime.md) — input + fixed-step loop
- [ADR-018](../decisions/ADR-018-dirty-rendering-and-frame-pacing.md) — dirty rendering +
  frame pacing/VRR
- [ADR-019](../decisions/ADR-019-power-aware-runtime.md) — power-aware defaults; no full-load loop
- [ADR-020](../decisions/ADR-020-window-resize-and-lifecycle.md) — resize/DPI/lifecycle separation
- [ADR-021](../decisions/ADR-021-async-runtime.md) — async/await; never block the UI thread
- [ADR-022](../decisions/ADR-022-diagnostics-and-profiling.md) — diagnostics/profiling
- [ADR-023](../decisions/ADR-023-os-event-normalization-and-foldables.md) — OS events/foldables

## 3. Specs (normative)

- [`specs/project-config-v1.md`](specs/project-config-v1.md) — `tessera.json` project schema
- [`specs/runtime-config-v1.md`](specs/runtime-config-v1.md) — FPS cap, vsync/VRR, pacing,
  dirty rendering, resize, async, deterministic mode
- [`specs/stylesheet-v1.md`](specs/stylesheet-v1.md) — full CSS syntax + semantic matrix
- [`specs/diagnostics-config-v1.md`](specs/diagnostics-config-v1.md) — logging, tracing,
  profiling, crash, overlay
- [`specs/os-events-v1.md`](specs/os-events-v1.md) — normalized OS events, display
  capabilities, mobile/foldables
- [`specs/ir-v1.md`](specs/ir-v1.md) — compiler IR schema, expressions, bindings, validation
- [`specs/component-model-v1.md`](specs/component-model-v1.md) — JSX/hooks subset, lifecycle,
  reconciliation, generated C++
- [`specs/golden-tests-v1.md`](specs/golden-tests-v1.md) — golden case/report format and
  comparison rules
- [`specs/shader-manifest-v1.md`](specs/shader-manifest-v1.md) — shared Vulkan/WebGPU shader
  interface
- [`specs/versioning-v1.md`](specs/versioning-v1.md) — SemVer, schema/IR/manifest versions,
  migrations
- [`specs/error-model-v1.md`](specs/error-model-v1.md) — error codes, severity, propagation,
  redaction
- [`specs/dependencies.md`](specs/dependencies.md) — canonical dependency and license map

## 4. Design

- [`design/renderer.md`](design/renderer.md) — RHI, batched 2D, Vulkan backend
- [`design/scene-graph.md`](design/scene-graph.md) — node tree, hit-testing, events, dirty bits
- [`design/ui-model.md`](design/ui-model.md) — components, retained scene, reconciliation, frame
- [`design/layout.md`](design/layout.md) — Yoga Flexbox, Grid subset, text measurement, rounding
- [`design/text.md`](design/text.md) — shaping, bidi, line breaking, glyph atlas
- [`design/animation.md`](design/animation.md) — transitions, keyframes, springs, fixed-step
  sampling
- [`design/responsive.md`](design/responsive.md) — breakpoints, safe areas, multi-window, fold
  postures
- [`design/accessibility.md`](design/accessibility.md) — semantics tree, platform bridges,
  focus, contrast/motion
- [`design/input.md`](design/input.md) — input normalization, focus, capture, IME
- [`design/game-loop.md`](design/game-loop.md) — fixed timestep, frame cap, vsync, VRR
- [`design/dirty-rendering.md`](design/dirty-rendering.md) — invalidation, caches, damage,
  multi-frame execution
- [`design/performance.md`](design/performance.md) — power-aware defaults, budgets, adaptive
  quality, hardware utilization
- [`design/window-resize.md`](design/window-resize.md) — resize/DPI/lifecycle, declarative vs
  infrastructure, OS compliance
- [`design/hal.md`](design/hal.md) — window/display/input/power/lifecycle abstraction
- [`design/async.md`](design/async.md) — C++20 coroutines, executors, cancellation, no blocking
- [`design/diagnostics.md`](design/diagnostics.md) — logging, tracing, metrics, profiling,
  crash, capture
- [`design/compiler.md`](design/compiler.md) — JSX/TSX -> IR -> C++ pipeline
- [`design/designer.md`](design/designer.md) — Wasm/WebGPU preview, IPC, hot reload
- [`design/threading-and-memory.md`](design/threading-and-memory.md) — threads, ownership,
  allocators, GPU memory, shutdown
- [`design/resources.md`](design/resources.md) — assets, caches, uploads, lifetime, hot reload
- [`design/services.md`](design/services.md) — i18n, navigation, storage, networking,
  notifications
- [`design/security.md`](design/security.md) — threat model, compiler/runtime/designer,
  supply chain, privacy
- [`design/testing.md`](design/testing.md) — test pyramid, determinism, goldens, CI
- [`design/build-and-packaging.md`](design/build-and-packaging.md) — CMake, static linking,
  assets, notices, SBOM, release

## 5. Guides

- [`guides/getting-started.md`](guides/getting-started.md) — build, run tests/demos,
  troubleshoot
- [`guides/project-generator.md`](guides/project-generator.md) — create/configure/build a project
- [`guides/developer-workflow.md`](guides/developer-workflow.md) — generated-code ownership,
  protected regions, regeneration
- [`guides/code-style.md`](guides/code-style.md) — style, file size, build-cache rules
- [`guides/testing.md`](guides/testing.md) — run/add tests, sanitizers, CI, triage
- [`guides/packaging.md`](guides/packaging.md) — release/static-link/notices/assets checklist
- [`guides/debugging.md`](guides/debugging.md) — validation, GPU tools, common failures

## 6. Reference

- [`reference/glossary.md`](reference/glossary.md) — terms and abbreviations

## 7. Reading orders

### New contributor

1. [Architecture overview](architecture/overview.md)
2. [Module map](architecture/module-map.md)
3. [Getting started](guides/getting-started.md)
4. [Code style](guides/code-style.md)
5. [Developer workflow](guides/developer-workflow.md)
6. [Testing](guides/testing.md)

### Runtime / UI

1. [Stylesheet spec](specs/stylesheet-v1.md)
2. [Scene graph](design/scene-graph.md)
3. [UI model](design/ui-model.md)
4. [Layout](design/layout.md)
5. [Text](design/text.md)
6. [Animation](design/animation.md)
7. [Responsive](design/responsive.md)
8. [Accessibility](design/accessibility.md)
9. [Input handler](design/input.md)
10. [Game loop](design/game-loop.md)
11. [Dirty rendering](design/dirty-rendering.md)
12. [Performance & power](design/performance.md)
13. [Window resize & lifecycle](design/window-resize.md)
14. [OS events & foldables](specs/os-events-v1.md)
15. [HAL](design/hal.md)
16. [Async runtime](design/async.md)
17. [Diagnostics](design/diagnostics.md)
18. [Runtime config](specs/runtime-config-v1.md)
19. [Diagnostics config](specs/diagnostics-config-v1.md)

### Compiler / codegen

1. [Compiler](design/compiler.md)
2. [IR spec](specs/ir-v1.md)
3. [Component model](specs/component-model-v1.md)
4. [Project generator](guides/project-generator.md)
5. [Developer workflow](guides/developer-workflow.md)
6. [Testing](guides/testing.md)

### Platform / HAL

1. [HAL](design/hal.md)
2. [Window resize](design/window-resize.md)
3. [OS events](specs/os-events-v1.md)
4. [Runtime config](specs/runtime-config-v1.md)
5. [Async](design/async.md)
6. [Diagnostics](design/diagnostics.md)

### Design-time viewer

1. [Designer](design/designer.md)
2. [Compiler](design/compiler.md)
3. [IR](specs/ir-v1.md)
4. [Renderer](design/renderer.md)
5. [Shader manifest](specs/shader-manifest-v1.md)
6. [Security](design/security.md)

## 8. Status

| Area | State |
|---|---|
| RHI + Vulkan batched 2D | B1 implemented |
| Scene graph + events | B1 implemented |
| Protected-region merger | B1 prototype |
| Stylesheet engine | B2 proposed (ADR-016) |
| Yoga layout / text / glyph atlas | B2 planned |
| Input handler | B2 proposed (ADR-017) |
| Fixed-step loop | B2 proposed (ADR-017) |
| Dirty rendering + frame pacing | B2/B3 proposed (ADR-018) |
| Power-aware idle/adaptive defaults | B2 proposed (ADR-019) |
| Window resize/DPI/lifecycle | B3 proposed (ADR-020) |
| Async/await runtime | B2 proposed (ADR-021) |
| Diagnostics/profiling | B2 proposed (ADR-022) |
| OS events / foldables | B3 proposed (ADR-023) |
| Compiler/IR/codegen | C proposed |
| Designer/Wasm | D proposed |
| Accessibility/services | F proposed |
| Packaging/release | F proposed |
| Pixel goldens on lavapipe/software | B2/B3 planned |
