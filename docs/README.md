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
- [ADR-024](../decisions/ADR-024-native-first-boundaries.md) — native-first; no production bridges
- [ADR-025](../decisions/ADR-025-error-boundaries.md) — error boundaries/exception isolation
- [ADR-026](../decisions/ADR-026-deep-links-and-external-activation.md) — deep links/activation
- [ADR-027](../decisions/ADR-027-agent-automation-and-audits.md) — numeric IDs/headless/audits
- [ADR-028](../decisions/ADR-028-custom-native-elements.md) — custom native elements/3D/input
- [ADR-029](../decisions/ADR-029-interaction-completeness.md) — text editing/scroll/overlays/forms
- [ADR-030](../decisions/ADR-030-app-platform-completeness.md) — navigation/restore/shell/i18n/a11y/data
- [ADR-031](../decisions/ADR-031-production-tooling-and-distribution.md) — devtools/updates/crash
- [ADR-032](../decisions/ADR-032-bundled-component-library.md) — component library/design tokens

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
- [`specs/virtualization-v1.md`](specs/virtualization-v1.md) — virtualized lists/grids,
  recycling, scroll anchoring, accessibility
- [`specs/memory-model-v1.md`](specs/memory-model-v1.md) — ownership, allocators, frame/GPU
  lifetime, caches, bridge memory, budgets
- [`specs/buffer-config-v1.md`](specs/buffer-config-v1.md) — buffer defaults, user tunables,
  ranges/scopes, memory budget, apply/restart rules
- [`specs/resource-lifecycle-v1.md`](specs/resource-lifecycle-v1.md) — lazy/preload/lease/
  unload/eviction, budgets, determinism
- [`specs/error-boundaries-v1.md`](specs/error-boundaries-v1.md) — caught/not-caught errors,
  fallback/reset/retry, async errors, exception policy
- [`specs/deep-linking-v1.md`](specs/deep-linking-v1.md) — external activation, route mapping,
  single instance, auth gate, untrusted-link security
- [`specs/automation-ids-v1.md`](specs/automation-ids-v1.md) — numeric UiId/ComponentId,
  manifest, snapshots, trace correlation
- [`specs/headless-automation-v1.md`](specs/headless-automation-v1.md) — deterministic harness,
  scripts/actions/asserts, agent protocol
- [`specs/lighthouse-audit-v1.md`](specs/lighthouse-audit-v1.md) — response-time metrics,
  categories, scoring, reports, CI thresholds
- [`specs/custom-elements-v1.md`](specs/custom-elements-v1.md) — JSX-declared native elements,
  RHI/shader API, 3D composition, element input, reuse, headless tests
- [`specs/text-editing-v1.md`](specs/text-editing-v1.md) — selection/caret/IME/undo/clipboard,
  TextInput/TextArea, secure fields
- [`specs/scroll-and-gestures-v1.md`](specs/scroll-and-gestures-v1.md) — scroll physics, nested
  scroll, gesture arena, sticky/anchoring
- [`specs/overlays-and-layers-v1.md`](specs/overlays-and-layers-v1.md) — overlay/modal/popover/
  tooltip/toast layers, focus/dismiss/anchoring
- [`specs/forms-and-validation-v1.md`](specs/forms-and-validation-v1.md) — field state, sync/async
  validation, submit/focus/reset
- [`specs/navigation-v1.md`](specs/navigation-v1.md) — route table, back stack, guards, modal
  routes, deep-link/startup integration
- [`specs/state-restoration-v1.md`](specs/state-restoration-v1.md) — versioned route/window/scroll/
  focus/draft restoration
- [`specs/platform-shell-v1.md`](specs/platform-shell-v1.md) — clipboard, drag/drop, context
  menus, file dialogs, menu bar/tray
- [`specs/i18n-v1.md`](specs/i18n-v1.md) — locale negotiation, ICU catalogs, formatting, RTL,
  pseudo-localization
- [`specs/accessibility-v1.md`](specs/accessibility-v1.md) — semantics/roles/actions/focus/
  announcements/platform bridges/tests
- [`specs/async-data-v1.md`](specs/async-data-v1.md) — resource cache, fetch policy, mutations,
  offline, pagination
- [`specs/devtools-protocol-v1.md`](specs/devtools-protocol-v1.md) — local inspector, events,
  screenshots/traces, hot reload
- [`specs/updates-and-distribution-v1.md`](specs/updates-and-distribution-v1.md) — signed
  manifest, staging/rollback, signing/notarization, channels
- [`specs/crash-reporting-v1.md`](specs/crash-reporting-v1.md) — dumps, symbols, version
  matching, opt-in upload, redaction
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
- [`design/antialiasing.md`](design/antialiasing.md) — current SDF AA, text/path/MSAA gaps
- [`design/startup-and-loading.md`](design/startup-and-loading.md) — startup, loading screen,
  auth gate, route load, failure/offline
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
- [`design/mobile-integration.md`](design/mobile-integration.md) — Android/iOS surface, input,
  IME, accessibility, lifecycle, and manifest/Info.plist declarations
- [`design/bridges.md`](design/bridges.md) — native-first bridge policy (JNI/ObjC/Wasm/IPC)
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
- [`design/component-library.md`](design/component-library.md) — tokens, accessible primitives,
  data grid, packaging, roadmap
- [`design/media-and-export.md`](design/media-and-export.md) — video/audio seams, printing, PDF,
  image/vector export

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
- [`guides/tunables.md`](guides/tunables.md) — expose a few buffer/runtime knobs to users
- [`guides/loading-matrix.md`](guides/loading-matrix.md) — choose startup/loading/auth/resource
  strategy by app profile
- [`guides/deep-linking.md`](guides/deep-linking.md) — declare schemes/hosts/routes and test
  browser/file activation
- [`guides/agent-automation.md`](guides/agent-automation.md) — use IDs/snapshots/actions/traces
  and audits from a coding agent
- [`guides/custom-elements.md`](guides/custom-elements.md) — declare/implement/test a custom
  native element with shaders, 3D, input and reuse
- [`guides/sample-apps.md`](guides/sample-apps.md) — templates and feature-coverage matrix
  (notes/dashboard/settings/3D/chat)

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
4. [UI virtualization](specs/virtualization-v1.md)
5. [Layout](design/layout.md)
6. [Text](design/text.md)
7. [Animation](design/animation.md)
8. [Responsive](design/responsive.md)
9. [Accessibility](design/accessibility.md)
10. [Input handler](design/input.md)
11. [Game loop](design/game-loop.md)
12. [Dirty rendering](design/dirty-rendering.md)
13. [Performance & power](design/performance.md)
14. [Window resize & lifecycle](design/window-resize.md)
15. [OS events & foldables](specs/os-events-v1.md)
16. [HAL](design/hal.md)
17. [Mobile integration](design/mobile-integration.md)
18. [Async runtime](design/async.md)
19. [Diagnostics](design/diagnostics.md)
20. [Runtime config](specs/runtime-config-v1.md)
21. [Diagnostics config](specs/diagnostics-config-v1.md)
22. [Memory model](specs/memory-model-v1.md)
23. [Buffer config & tunables](specs/buffer-config-v1.md)
24. [Tunables guide](guides/tunables.md)
25. [Bridges](design/bridges.md)

### Startup / loading / auth

1. [Startup & loading design](design/startup-and-loading.md)
2. [Resource lifecycle](specs/resource-lifecycle-v1.md)
3. [Error boundaries](specs/error-boundaries-v1.md)
4. [Deep linking](specs/deep-linking-v1.md)
5. [Loading strategy matrix](guides/loading-matrix.md)
6. [Resources & assets](design/resources.md)
7. [Async runtime](design/async.md)
8. [Performance & power](design/performance.md)
9. [Security](design/security.md)

### Custom native elements

1. [Custom elements spec](specs/custom-elements-v1.md)
2. [Custom elements guide](guides/custom-elements.md)
3. [IR](specs/ir-v1.md)
4. [Shader manifest](specs/shader-manifest-v1.md)
5. [Renderer](design/renderer.md)
6. [Input](design/input.md)
7. [Headless automation](specs/headless-automation-v1.md)

### App interaction

1. [Text editing](specs/text-editing-v1.md)
2. [Scroll and gestures](specs/scroll-and-gestures-v1.md)
3. [Overlays and layers](specs/overlays-and-layers-v1.md)
4. [Forms and validation](specs/forms-and-validation-v1.md)
5. [Input](design/input.md)
6. [Text](design/text.md)

### App platform

1. [Navigation](specs/navigation-v1.md)
2. [State restoration](specs/state-restoration-v1.md)
3. [Platform shell](specs/platform-shell-v1.md)
4. [i18n](specs/i18n-v1.md)
5. [Accessibility](specs/accessibility-v1.md)
6. [Async data](specs/async-data-v1.md)
7. [Services](design/services.md)

### Production readiness

1. [Devtools protocol](specs/devtools-protocol-v1.md)
2. [Updates and distribution](specs/updates-and-distribution-v1.md)
3. [Crash reporting](specs/crash-reporting-v1.md)
4. [Component library](design/component-library.md)
5. [Sample apps](guides/sample-apps.md)
6. [Packaging](guides/packaging.md)

### Automation / observability

1. [Numeric automation IDs](specs/automation-ids-v1.md)
2. [Headless automation](specs/headless-automation-v1.md)
3. [Agent automation guide](guides/agent-automation.md)
4. [Lighthouse-style audits](specs/lighthouse-audit-v1.md)
5. [Diagnostics](design/diagnostics.md)
6. [Diagnostics config](specs/diagnostics-config-v1.md)
7. [Testing](guides/testing.md)

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
| Anti-aliasing | B1 partial: SDF rounded rects only |
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
| UI virtualization (list/grid) | B2/E proposed |
| Mobile integration (Android first) | design only |
| Memory model / bridge policy | proposed / ADR-024 accepted |
| Buffer config / user tunables | B2/B3 proposed |
| Startup / loading / auth | design only |
| Resource lifecycle (lazy/preload/unload) | B2/F proposed |
| Error boundaries | C/F proposed (ADR-025 accepted) |
| Deep links / external activation | F proposed (ADR-026 accepted) |
| Numeric automation IDs / headless | C proposed (ADR-027 accepted) |
| Lighthouse-style audits | B2/F proposed (ADR-027 accepted) |
| Custom native elements / 3D | B2/C/D proposed (ADR-028 accepted) |
| Text editing / forms | B2/C proposed (ADR-029 accepted) |
| Scroll / gestures | B2 proposed (ADR-029 accepted) |
| Overlays / layers | B2/C proposed (ADR-029 accepted) |
| Navigation / state restore | F proposed (ADR-030 accepted) |
| Platform shell / i18n | F proposed (ADR-030 accepted) |
| Accessibility | F proposed (ADR-030 accepted) |
| Async data / offline | F proposed (ADR-030 accepted) |
| Devtools / hot reload | C/D proposed (ADR-031 accepted) |
| Updates / signing / crash | F proposed (ADR-031 accepted) |
| Component library | post-V1 proposed (ADR-032 accepted) |
| Samples / media / export | planned/deferred |
| Designer/Wasm | D proposed |
| Packaging/release | F proposed |
| Pixel goldens on lavapipe/software | B2/B3 planned |
