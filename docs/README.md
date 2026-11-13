# Tessera documentation map

Start with [`../AGENTS.md`](../AGENTS.md) for the operating rules and [`../README.md`](../README.md)
for the project overview. Contribution workflow: [`../CONTRIBUTING.md`](../CONTRIBUTING.md).
This page is the map and reading order for the framework.

## 0. Documentation rules

- One topic per file; target 400 lines, hard limit 600 lines.
- A topic over 600 lines becomes a folder with a `README.md` index and linked parts.
- New/edited docs follow this rule; an existing oversized document is split when next edited.
- Update this map and the relevant folder index whenever a doc is added or moved.

## 1. Architecture

- [`architecture/overview.md`](architecture/overview.md) — layers, pipelines, threading,
  determinism, status.
- [`architecture/app-completeness-cross-check-v1.md`](architecture/app-completeness-cross-check-v1.md)
  — cross-check, canonical ownership, gap register, open questions
- [`architecture/app-completeness-gap-resolutions-v1.md`](architecture/app-completeness-gap-resolutions-v1.md)
  — resolved/deferred decisions for gaps G-01..G-28 and open questions Q-01..Q-26 (defined in the cross-check above)
- [`architecture/app-framework-gap-audit-v1.md`](architecture/app-framework-gap-audit-v1.md)
  — app-framework concerns (lifecycle, services, provisioning, app state, storage, background work)
  specified vs implemented, contradictions, and a Qt/JUCE/Flutter/Compose comparison (ADR-050)
- [`architecture/doc-contradiction-resolutions-v1.md`](architecture/doc-contradiction-resolutions-v1.md)
  — contradictions found between documents, the rule used to resolve each, and what is still undecided
- [`architecture/module-map.md`](architecture/module-map.md) — source tree, build targets,
  ownership, dependency direction.
- [`architecture/implementation-log.md`](architecture/implementation-log.md) — historical status
  narrative (formerly AGENTS.md "Current focus"); the worklist is the milestone-mapped Big Steps.

## 2. Decisions

[`../decisions/README.md`](../decisions/README.md) is the ADR index. The list below covers [ADR-005](../decisions/ADR-005-yoga-layout-adapter.md) to [ADR-009](../decisions/ADR-009-deterministic-headless-golden-tests.md) and [ADR-016](../decisions/ADR-016-stylesheet-engine.md) to [ADR-037](../decisions/ADR-037-self-contained-wasm-designer.md) (layout, text, compiler, goldens, runtime, app-level, and design-time ADRs). [ADR-001](../decisions/ADR-001-vulkan-1-3-baseline.md) to [ADR-004](../decisions/ADR-004-buy-vector-path-rasterization.md) (rendering baseline) and [ADR-010](../decisions/ADR-010-macos-via-moltenvk-deferred.md) to [ADR-015](../decisions/ADR-015-code-style-build-cache.md) (macOS, shader authoring, static linking, license, project config, code style) appear only in the index:

- [ADR-005](../decisions/ADR-005-yoga-layout-adapter.md) — Yoga Flexbox + small Grid subset
- [ADR-006](../decisions/ADR-006-text-stack.md) — FreeType/HarfBuzz/SheenBidi/ICU
- [ADR-007](../decisions/ADR-007-compiler-ir.md) — neutral IR (intermediate representation) before codegen
- [ADR-008](../decisions/ADR-008-protected-regions-and-manifest.md) — [protected regions](reference/glossary.md)/merge
- [ADR-009](../decisions/ADR-009-deterministic-headless-golden-tests.md) — deterministic [goldens](reference/glossary.md)
- [ADR-016](../decisions/ADR-016-stylesheet-engine.md) — stylesheet engine
- [ADR-017](../decisions/ADR-017-input-and-fixed-step-runtime.md) — input + fixed-step loop
- [ADR-018](../decisions/ADR-018-dirty-rendering-and-frame-pacing.md) — dirty rendering +
  [frame pacing](reference/glossary.md)/VRR
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
- [ADR-033](../decisions/ADR-033-app-completeness-gap-resolutions.md) — gap resolutions/deferrals
- [ADR-034](../decisions/ADR-034-interaction-and-overscroll-animation.md) — interaction/overscroll animation
- [ADR-035](../decisions/ADR-035-view-transitions.md) — view transitions (global UI effects)
- [ADR-036](../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md) — own JSX/TSX front end; React-like model
- [ADR-037](../decisions/ADR-037-self-contained-wasm-designer.md) — self-contained Wasm (WebAssembly) designer engine
- [ADR-039](../decisions/ADR-039-lowercase-html-tags-accepted.md) — lowercase HTML tags accepted as intrinsic aliases
- [ADR-050](../decisions/ADR-050-opt-in-app-framework-layer.md) — application-framework layer (lifecycle, services resolved at mount, `useService<Interface>()`, task scopes, app state, native render surfaces); proposed
- [ADR-051](../decisions/ADR-051-ir-record-types-and-record-values.md) — IR record types and record values (accepted; object-literal prop defaults, #158)
- [ADR-052](../decisions/ADR-052-networking-platform-stacks.md) — networking on platform HTTP stacks (accepted; #197)
- [ADR-053](../decisions/ADR-053-native-metal-backend.md) — native Metal backend for macOS/iOS (accepted; supersedes ADR-010 in part)

## 3. Specs (normative)

- [`specs/project-config-v1.md`](specs/project-config-v1.md) — `tessera.json` project schema
- [`specs/runtime-config-v1.md`](specs/runtime-config-v1.md) — FPS cap, vsync/VRR, pacing,
  dirty rendering, resize, async, deterministic mode
- [`specs/stylesheet-v1.md`](specs/stylesheet-v1.md) — full CSS syntax + semantic matrix
- [`specs/compiled-stylesheet-v1.md`](specs/compiled-stylesheet-v1.md) — precompiled stylesheet blob format (prepare/resolve split, ADR-048)
- [`specs/diagnostics-config-v1.md`](specs/diagnostics-config-v1.md) — logging, tracing,
  profiling, crash, overlay
- [`specs/os-events-v1.md`](specs/os-events-v1.md) — normalized OS events, display
  capabilities, mobile/foldables
- [`specs/ir-v1.md`](specs/ir-v1.md) — compiler IR schema, expressions, bindings, validation
- [`specs/ir-v1-example.md`](specs/ir-v1-example.md) — the worked Counter `ir.json` example
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
- [`specs/startup-api-v1.md`](specs/startup-api-v1.md) — developer startup surface: `App` root,
  replaceable framework loading screen, prioritized resource hints, launch modes, phases P0-P5
- [`specs/startup-tests-v1.md`](specs/startup-tests-v1.md) — acceptance tests of the startup
  surface, launch modes and stored state
- [`specs/window-chrome-v1.md`](specs/window-chrome-v1.md) — title bar, menu bar and status/navigation
  bars: appearance policy, launch phases, insets, platform support
- [`specs/error-boundaries-v1.md`](specs/error-boundaries-v1.md) — caught/not-caught errors,
  fallback/reset/retry, async errors, exception policy
- [`specs/deep-linking-v1.md`](specs/deep-linking-v1.md) — external activation, route mapping,
  single instance, auth gate, untrusted-link security
- [`specs/automation-ids-v1.md`](specs/automation-ids-v1.md) — numeric UiId/ComponentId,
  manifest, snapshots, trace correlation
- [`specs/headless-automation-v1.md`](specs/headless-automation-v1.md) — deterministic harness,
  scripts/actions/asserts, agent protocol
- [`specs/lighthouse-audit-v1.md`](specs/lighthouse-audit-v1.md) — response-time metrics,
  categories, scoring, reports, CI (continuous integration) thresholds
- [`specs/custom-elements-v1.md`](specs/custom-elements-v1.md) — JSX-declared native elements,
  RHI (render hardware interface)/shader API, 3D composition, element input, reuse, headless tests
- [`specs/text-editing-v1.md`](specs/text-editing-v1.md) — selection/caret/IME/undo/clipboard,
  TextInput/TextArea, secure fields
- [`specs/scroll-and-gestures-v1.md`](specs/scroll-and-gestures-v1.md) — scroll physics, nested
  scroll, gesture arena, sticky/anchoring
- [`specs/animation-v1.md`](specs/animation-v1.md) — transitions/springs, interaction feedback,
  scroll fling/snap, overscroll stretch/spring-back, reduced motion
- [`specs/designer-protocol-v1.md`](specs/designer-protocol-v1.md) — host/viewer messages of the
  browser designer: source sync, picker, style overrides, commit as text patches
- [`specs/designer-studio-v1.md`](specs/designer-studio-v1.md) — hosted studio: GitHub Pages deploy,
  page-as-host, skins and simulated system chrome, project import/export, MCP stdio mode
- [`specs/jsx-subset-v1.md`](specs/jsx-subset-v1.md) — accepted JSX/TSX syntax: grammar, expressions,
  handlers, hooks (sections 0-9)
- [`specs/jsx-subset-v1-jsx.md`](specs/jsx-subset-v1-jsx.md) — part 2 of the JSX/TSX subset: JSX,
  intrinsic elements, rejected constructs, diagnostics, error-tolerant parsing, limits (sections 10-18)
- [`specs/jsx-intrinsics-v1/`](specs/jsx-intrinsics-v1/README.md) — intrinsic element table: children,
  props, events and payloads, refs, IR lowering, lowercase HTML aliases, React compatibility,
  machine-readable form (folder with index)
- [`specs/view-transitions-v1.md`](specs/view-transitions-v1.md) — window-wide transitions
  (`startViewTransition`, theme circle reveal, shader effects like glitch)
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
- [`specs/i18n-v1.md`](specs/i18n-v1.md) — locale negotiation, ICU (International Components for Unicode) catalogs, formatting, RTL,
  pseudo-localization
- [`specs/accessibility-v1.md`](specs/accessibility-v1.md) — semantics/roles/actions/focus/
  announcements/platform bridges/tests
- [`specs/networking-v1.md`](specs/networking-v1.md) — the `net` module: `fetch`-shaped interface, normative behavior, back ends and contract suite (ADR-052)
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
- [`specs/versioning-v1.md`](specs/versioning-v1.md) — SemVer (semantic versioning), schema/IR/manifest versions,
  migrations
- [`specs/error-model-v1.md`](specs/error-model-v1.md) — error codes, severity, propagation,
  redaction
- [`specs/dependencies.md`](specs/dependencies.md) — canonical dependency and license map

## 4. Design

- [`design/renderer.md`](design/renderer.md) — B1 renderer summary, batched 2D, Vulkan backend
- [`design/paint-pass.md`](design/paint-pass.md) — computed-style box paint: background/border
  quads, rounded overflow clips, `PaintData` text runs, source coordinates, sink seam, limits
- [`design/rendering-production-plan.md`](design/rendering-production-plan.md) — production plan:
  static-linking and license gates, rendering workstreams, dependency matrix
- [`design/thorvg-spike/README.md`](design/thorvg-spike/README.md) — ThorVG P1 spike and the path
  pipeline: pinned bring-up, `PathRasterizer`, texture cache, SVG decode/document cache,
  measurements, Go decision
- [`design/gpu/README.md`](design/gpu/README.md) — GPU docs index: RHI, frames/sync, resources,
  pipelines, backends, tests
- [`design/gpu/rhi.md`](design/gpu/rhi.md) — device/resource/command contract, lifetime, errors
- [`design/gpu/frames.md`](design/gpu/frames.md) — frame lifecycle, frames in flight, barriers,
  uploads/readbacks, resize
- [`design/gpu/resources.md`](design/gpu/resources.md) — buffers/textures/targets/samplers/memory
- [`design/gpu/pipelines.md`](design/gpu/pipelines.md) — pipeline cache, descriptors, vertex
  layout, shaders
- [`design/gpu/backends.md`](design/gpu/backends.md) — Vulkan/WebGPU/null/software parity and
  capabilities
- [`design/gpu/webgpu-backend.md`](design/gpu/webgpu-backend.md) — WebGPU backend first slice: RHI
  requirements, mapping tables, WGSL registry, unbuilt device skeleton
- [`design/gpu/testing.md`](design/gpu/testing.md) — GPU test pyramid/tree, goldens, CI
- [`design/antialiasing.md`](design/antialiasing.md) — current SDF (signed distance field) AA, text/path/MSAA gaps
- [`design/startup-and-loading.md`](design/startup-and-loading.md) — startup, loading screen,
  auth gate, route load, failure/offline
- [`design/scene-graph.md`](design/scene-graph.md) — node tree, hit-testing, events, dirty bits
- [`design/click-synthesis.md`](design/click-synthesis.md) — `onClick` as a synthesized `Click` (pointer, keyboard, accessibility)
- [`design/ui-model.md`](design/ui-model.md) — components, retained scene, reconciliation, frame
- [`design/layout.md`](design/layout.md) — Yoga Flexbox, Grid subset, text measurement, rounding
- [`design/layout-observation.md`](design/layout-observation.md) — size observation (`onSizeChange`), layout read model, settle rounds, container queries plan (design only, ADR-041 accepted)
- [`design/text.md`](design/text.md) — shaping, bidi, line breaking, glyph atlas, paint-time runs
- [`design/icu-data.md`](design/icu-data.md) — ICU reduced static data spike: measured blob,
  locale subset, build cost, go/no-go
- [`design/animation.md`](design/animation.md) — transitions, keyframes, springs, fixed-step
  sampling
- [`design/animation-runtime.md`](design/animation-runtime.md) — implemented runtime: module map,
  tick ordering, pause/fills/stop/restart, loop bridge, reduced motion, test map
- [`design/responsive.md`](design/responsive.md) — breakpoints, safe areas, multi-window, fold
  postures
- [`design/accessibility.md`](design/accessibility.md) — semantics tree, platform bridges,
  focus, contrast/motion
- [`design/input.md`](design/input.md) — input normalization, focus, capture, IME (input method editor)
- [`design/game-loop.md`](design/game-loop.md) — fixed timestep, frame cap, vsync, VRR (variable refresh rate)
- [`design/app-host.md`](design/app-host.md) — the production frame loop that wires style, layout, sync, damage, paint and present (steps 3-8 implemented by `tessera_app`; input/present are the driver)
- [`design/shell-design-preview.md`](design/shell-design-preview.md) — mock captures of the startup P0/P2/P4 frames and the system vs framework window chrome, rendered through the real style/layout/paint path
- [`design/dirty-rendering.md`](design/dirty-rendering.md) — invalidation, caches, damage,
  multi-frame execution, render-side damage present-loop evidence (`damage_min`/`damage_present`)
- [`design/performance.md`](design/performance.md) — power-aware defaults, budgets, adaptive
  quality, hardware utilization
- [`design/window-resize.md`](design/window-resize.md) — resize/DPI/lifecycle, declarative vs
  infrastructure, OS compliance, present-path hardening (`--msaa 4`, `--simulate-loss`)
- [`design/hal.md`](design/hal.md) — window/display/input/power/lifecycle abstraction
- [`design/mobile-integration.md`](design/mobile-integration.md) — Android/iOS surface, input,
  IME, accessibility, lifecycle, and manifest/Info.plist declarations
- [`design/bridges.md`](design/bridges.md) — native-first bridge policy (JNI (Java Native Interface)/ObjC/Wasm/IPC)
- [`design/async.md`](design/async.md) — C++20 coroutines, executors, cancellation, no blocking
- [`design/diagnostics.md`](design/diagnostics.md) — logging, tracing, metrics, profiling,
  crash, capture
- [`design/error-boundaries.md`](design/error-boundaries.md) — routing classes, RTTI-free catch
  order, retry state machine, cleanup seam, not-caught list, test map
- [`design/compiler.md`](design/compiler.md) — JSX/TSX -> IR -> C++ pipeline
- [`design/jsx-frontend.md`](design/jsx-frontend.md) — own JSX/TSX lexer/parser: slice plan,
  modes, diagnostics, spec gaps decided
- [`design/jsx-ir-lowering.md`](design/jsx-ir-lowering.md) — AST to IR lowering design: construct
  mapping, analysis pass, spans, bind ids, layout, tests, slices, open questions
- [`design/designer.md`](design/designer.md) — Wasm/WebGPU preview, IPC (inter-process communication), hot reload
- [`design/threading-and-memory.md`](design/threading-and-memory.md) — threads, ownership,
  allocators, GPU memory, shutdown
- [`design/resources.md`](design/resources.md) — assets, caches, uploads, lifetime, hot reload
- [`design/services.md`](design/services.md) — i18n (internationalization), navigation, storage, networking,
  notifications
- [`design/native-render-surfaces.md`](design/native-render-surfaces.md) — typed refs to native
  render surfaces drawn through the RHI: lifecycle, threading, dirty/idle rules (ADR-050 section 7)
- [`design/service-resolve-benchmark.md`](design/service-resolve-benchmark.md) — measured cost of
  resolving app services (per call, per mount, cache effects, virtual vs static dispatch) behind
  ADR-050's resolve-at-mount rule
- [`design/app-framework-open-questions.md`](design/app-framework-open-questions.md) — proposed
  answers to ADR-050's open questions (background work, scopes, multi-window, SQLite, HTTP, state,
  startup, default modules, surfaces) and the spec contradictions they would fix
- [`design/reference-equantic-ui.md`](design/reference-equantic-ui.md) — reference study of
  eQuantic.UI (C# web + native GPU engine): architecture, maturity, lessons, playground slice plan
- [`design/security.md`](design/security.md) — threat model, compiler/runtime/designer,
  supply chain, privacy
- [`design/testing.md`](design/testing.md) — test pyramid, determinism, goldens, CI
- [`design/build-and-packaging.md`](design/build-and-packaging.md) — CMake, static linking,
  assets, notices, SBOM (software bill of materials), release
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
- [`guides/real-gpu-ci.md`](guides/real-gpu-ci.md) — the manual real-GPU lane: what it runs, the self-hosted runner setup, and the security rules for a public repo
- [`guides/code-style.md`](guides/code-style.md) — style, file size, build-cache rules
- [`guides/testing.md`](guides/testing.md) — run/add tests, sanitizers, CI, triage
- [`guides/packaging.md`](guides/packaging.md) — release checklist: gates, per-OS install/package,
  artifact names, manual signing/notarization
- [`guides/debugging.md`](guides/debugging.md) — validation, GPU tools, common failures
- [`guides/tunables.md`](guides/tunables.md) — expose a few buffer/runtime knobs to users
- [`guides/runtime-config-examples.md`](guides/runtime-config-examples.md) — worked
  `runtime` blocks per target with the resulting frame-cap behaviour
- [`guides/loading-matrix.md`](guides/loading-matrix.md) — choose startup/loading/auth/resource
  strategy by app profile
- [`guides/deep-linking.md`](guides/deep-linking.md) — declare schemes/hosts/routes and test
  browser/file activation
- [`guides/agent-automation.md`](guides/agent-automation.md) — use IDs/snapshots/actions/traces
  and audits from a coding agent
- [`tools/wiki/README.md`](../tools/wiki/README.md) — static React client for the GitHub Wiki (guides with examples;
  design docs stay here in the source repo), themed with a Tessera stylesheet
- [`guides/custom-elements.md`](guides/custom-elements.md) — declare/implement/test a custom
  native element with shaders, 3D, input and reuse
- [`guides/sample-apps.md`](guides/sample-apps.md) — templates and feature-coverage matrix
  (notes/dashboard/settings/3D/chat)

## 6. Reference

- [`reference/glossary.md`](reference/glossary.md) — terms and abbreviations
- [`reference/open-questions.md`](reference/open-questions.md) — register of every `Open questions` section in the specs and design notes: link to the source, the recorded decision or an explicit deferral with its trigger, and the items that unblock the next implementation round

## 7. Reading orders

### New contributor

1. [Architecture overview](architecture/overview.md)
2. [Module map](architecture/module-map.md)
3. [App-completeness cross-check](architecture/app-completeness-cross-check-v1.md)
4. [Gap resolutions](architecture/app-completeness-gap-resolutions-v1.md)
5. [Getting started](guides/getting-started.md)
6. [Code style](guides/code-style.md)
7. [Developer workflow](guides/developer-workflow.md)
8. [Testing](guides/testing.md)

### Runtime / UI

1. [Stylesheet spec](specs/stylesheet-v1.md)
2. [Scene graph](design/scene-graph.md)
3. [UI model](design/ui-model.md)
4. [UI virtualization](specs/virtualization-v1.md)
5. [Layout](design/layout.md)
   - [Layout observation](design/layout-observation.md)
6. [Text](design/text.md)
7. [Animation](design/animation.md)
8. [Animation runtime](design/animation-runtime.md)
9. [Responsive](design/responsive.md)
10. [Accessibility](design/accessibility.md)
11. [Input handler](design/input.md)
12. [Game loop](design/game-loop.md)
13. [Dirty rendering](design/dirty-rendering.md)
14. [Performance & power](design/performance.md)
15. [Window resize & lifecycle](design/window-resize.md)
16. [OS events & foldables](specs/os-events-v1.md)
17. [HAL](design/hal.md)
18. [Mobile integration](design/mobile-integration.md)
19. [Async runtime](design/async.md)
20. [Diagnostics](design/diagnostics.md)
21. [Runtime config](specs/runtime-config-v1.md)
22. [Diagnostics config](specs/diagnostics-config-v1.md)
23. [Memory model](specs/memory-model-v1.md)
24. [Buffer config & tunables](specs/buffer-config-v1.md)
25. [Tunables guide](guides/tunables.md)
26. [Bridges](design/bridges.md)

### GPU rendering

1. [GPU docs index](design/gpu/README.md)
2. [RHI contract](design/gpu/rhi.md)
3. [Frame execution/sync](design/gpu/frames.md)
4. [Resources/memory](design/gpu/resources.md)
5. [Pipelines/shaders](design/gpu/pipelines.md)
6. [Backends/parity](design/gpu/backends.md)
7. [GPU testing](design/gpu/testing.md)

### Startup / loading / auth

1. [Startup & loading design](design/startup-and-loading.md)
2. [Startup API](specs/startup-api-v1.md)
3. [Window chrome & system bars](specs/window-chrome-v1.md)
4. [Resource lifecycle](specs/resource-lifecycle-v1.md)
5. [Error boundaries](specs/error-boundaries-v1.md)
6. [Deep linking](specs/deep-linking-v1.md)
7. [Loading strategy matrix](guides/loading-matrix.md)
8. [Resources & assets](design/resources.md)
9. [Async runtime](design/async.md)
10. [Performance & power](design/performance.md)
11. [Security](design/security.md)

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
3. [Animation](specs/animation-v1.md)
4. [Overlays and layers](specs/overlays-and-layers-v1.md)
5. [Forms and validation](specs/forms-and-validation-v1.md)
6. [Input](design/input.md)
7. [Text](design/text.md)

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
2. [Designer studio](specs/designer-studio-v1.md)
3. [Designer protocol](specs/designer-protocol-v1.md)
4. [Compiler](design/compiler.md)
5. [IR](specs/ir-v1.md)
6. [Renderer](design/renderer.md)
7. [Shader manifest](specs/shader-manifest-v1.md)
8. [Security](design/security.md)

## 8. Status

In the State column, B1, B2, and B3 are the phases of [Big Step B](../AGENTS.md#big-steps-group-work-dont-micro-step) (renderer core, runtime slice, window/swapchain/OS); C, D, E, and F are Big Steps C (compiler vertical slice), D (design-time preview), E (data-heavy UI), and F (production breadth), defined in the same AGENTS.md section. "Proposed" and "accepted" repeat the ADR status from the [ADR index](../decisions/README.md); "design only" means documentation exists and no code does. V1 is the first release scope described under Big Steps in AGENTS.md.

| Area | State |
|---|---|
| RHI + Vulkan batched 2D | B1 implemented; swapchain/present seam B3; device-loss status + one-shot recovery with generation guards (D7) landed; the windowed POC proves offscreen-4x resolve present (`--msaa 4`) and end-to-end loss recovery (`--simulate-loss`), and `present_hardening` covers 3 deterministic frames + 4x resolve + the loss cycle; render-side damage present loop proven (`begin_damaged`: 1 region / 3-26% area on small changes, pixel-identical to a same-sink full redraw, full-redraw fallback, idle renders nothing) |
| Anti-aliasing | B1 partial: SDF rounded rects + grayscale text coverage + analytic rounded-rect clip intersection with a scissor fast path; MSAA 4x implemented (opt-in; offscreen resolve bridge in the POC); no path AA for geometry edges |
| Vector paths (ThorVG, D1) | P1 implemented behind `TESSERA_ENABLE_THORVG` (default OFF; meson-only upstream so a CMake wrapper compiles pinned v1.1.3 sources): `rendering::PathRasterizer` + null backend, bounded `PathTextureCache` (rasterize -> RHI texture, D7-aware), static SVG decode into path geometry, embedded asset bundles generated by the host-side `cmake/embed_assets.cmake` script, scene-node vector painting (`PaintKind::Vector`) with GPU readback and a bit-exact CPU paint golden; the Linux CI test job enables ThorVG; next is codegen emission of vector payloads ([spike](design/thorvg-spike/README.md), [plan](design/rendering-production-plan.md)) |
| [Scene graph](reference/glossary.md) + events | B1 implemented; pointer boundary events, gesture arena, scroll offsets/clipping, node metadata (id/class/role/aria/automation), event bindings and the accessibility semantics snapshot landed |
| Box paint pass | B2 implemented: computed-style backgrounds/borders -> batched 2D, wired into `FramePipeline::paint_frame` when a frame is due, damage-clipped via the redraw plan, `overflow`/border-radius children clipped with rounded-rect clip intersection, text runs from `ui::PaintData` through `PaintSink::draw_text`/`TextRenderer` (D9), embedded vector assets from `PaintData::Vector` through `PaintSink::draw_vector` (D10), embedded raster images from `PaintData::Image` through `PaintSink::draw_image` (D12: stb_image decode, bounded decode/texture caches with D7 handling, authored-sRGB format rule, bit-exact CPU image golden) and linear gradients with rounded-clip and CPU/GPU parity (D13) — all with damage replay and deterministic CPU/GPU goldens ([design](design/paint-pass.md)) |
| Protected-region merger | B1 prototype |
| JSX/TSX front end ([ADR-036](../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)) | C implemented through codegen: lexer, JSX/expression/type/module parser, native handlers, `bind::` pairing, Tier 1 rules, lowering (expressions, hooks/bindings, nodes, conditionals/loops, components/imports), IR writer/validator coverage and a C++ codegen CLI with merger-based incremental regeneration and a sample screen that compiles; remaining: state interpolation/regeneration and controlled input wiring ([design](design/jsx-ir-lowering.md), [workflow](guides/developer-workflow.md)) |
| Stylesheet engine | B2 implemented: tokenizer/parser, selectors and specificity, cascade and computed style with inheritance, `@layer`/media/`@supports`, keyframes, `var()` substitution with fallback and cycle detection, and the text-layer properties (`letter-spacing`, `word-spacing`, `text-transform`, `text-overflow`) ([ADR-016](../decisions/ADR-016-stylesheet-engine.md)) |
| Yoga layout / text / glyph atlas | B2 implemented: Yoga adapter, FreeType/HarfBuzz shaping, glyph atlas and text batching; ICU segmentation is opt-in (`TESSERA_ICU_REDUCED_DATA`) and proven by the dedicated `icu-text` CI job in `.github/workflows/linux.yml` |
| Input handler | B2 implemented: normalized queue/coalescing with pointer kinds, per-pointer capture, focus/keyboard, wheel/text routing, Tab traversal, hover/active/`:focus-visible` and pointer over/out/enter/leave boundary events, per-tick `InputPump`, Windows touch/pen translation (WM_POINTER + WM_TOUCH), IME composition plumbing, bounded keyboard accelerator registry, gesture arena driven by the pump; Linux/Wayland touch, text editing and scrolling pending ([ADR-017](../decisions/ADR-017-input-and-fixed-step-runtime.md)) |
| Fixed-step loop | B2 implemented: accumulator, clock, `RuntimeLoop`, pacer ([ADR-017](../decisions/ADR-017-input-and-fixed-step-runtime.md)) |
| Dirty rendering + frame pacing | B2 implemented (dirty flags/damage/redraw plan/frame pipeline/pacer); B3 present landed on Win32/X11 (two frames in flight); the pacer follows a scripted cap/refresh/VRR rate change, platform refresh/VRR queries and present-path damage pending ([ADR-018](../decisions/ADR-018-dirty-rendering-and-frame-pacing.md)) |
| Power-aware idle/adaptive defaults | B2 partial: render-on-demand idle implemented; adaptive quality pending ([ADR-019](../decisions/ADR-019-power-aware-runtime.md)) |
| Window resize/DPI/lifecycle | B3 implemented on Windows/X11 (resize/DPI/focus/close, swapchain retirement); Wayland and macOS pending ([ADR-020](../decisions/ADR-020-window-resize-and-lifecycle.md)) |
| Async/await runtime | B2 partial: cancellation/executor/completion queue and lazy coroutine `Task` implemented; I/O executor pending ([ADR-021](../decisions/ADR-021-async-runtime.md)) |
| Diagnostics/profiling | B2 partial: per-frame counters implemented; tracing/profiling/crash pending ([ADR-022](../decisions/ADR-022-diagnostics-and-profiling.md)) |
| Static-link + license gates | Production gates implemented: dependency/notice allowlist checker, per-OS static-link allowlist, forbidden dynamic C++ runtime rule, Release static-runtime proof, full license texts + generated bundle, deterministic SPDX 2.3 SBOM, byte-reproducible CPack archives (SOURCE_DATE_EPOCH/TZ) shipping SHA256SUMS + RELEASE-NOTES with content and checksum validation, install/package gates on Linux, Windows and macOS, docs link/index/fence/size gate, pinned formatter version gate, fixtures and CI wiring ([plan](design/rendering-production-plan.md), [packaging](guides/packaging.md)) |
| OS events / foldables | B3 accepted 2026-11-13, not yet implemented ([ADR-023](../decisions/ADR-023-os-event-normalization-and-foldables.md)) |
| Compiler/IR/codegen | C implemented: IR model, JSON writer/validator, full lowering chain, `tessera_schema` intrinsic/style table, C++ codegen (static tree, hooks, handlers, refs/effects, event wiring, protected regions) conditional branches and keyed-list templates (bounded, hash-first updates, stable ids), inline styles, and the `tessera_codegen` CLI with `--check`/`--write` regeneration; the generated sample compiles and a headless integration demo proves click -> state -> repaint with CPU goldens ([design](design/compiler.md), [demo](guides/developer-workflow.md)) |
| UI virtualization (list/grid) | B2/E proposed |
| Mobile integration (Android first) | design only |
| Memory model / bridge policy | proposed / [ADR-024](../decisions/ADR-024-native-first-boundaries.md) accepted |
| Buffer config / user tunables | B2/B3 proposed |
| Startup / loading / auth | design only; startup surface, chrome and Windows custom title bar specs proposed with owner decisions recorded (`startup-api-v1.md`, `window-chrome-v1.md`, [ADR-042](../decisions/ADR-042-jsx-startup-surface.md), [ADR-043](../decisions/ADR-043-launch-modes-and-state-reuse.md), [ADR-044](../decisions/ADR-044-system-window-chrome-in-v1.md)) |
| Resource lifecycle (lazy/preload/unload) | B2/F proposed |
| Error boundaries | C/F proposed ([ADR-025](../decisions/ADR-025-error-boundaries.md) accepted) |
| Deep links / external activation | F proposed ([ADR-026](../decisions/ADR-026-deep-links-and-external-activation.md) accepted) |
| Numeric automation IDs / headless | C proposed ([ADR-027](../decisions/ADR-027-agent-automation-and-audits.md) accepted) |
| Lighthouse-style audits | B2/F proposed ([ADR-027](../decisions/ADR-027-agent-automation-and-audits.md) accepted) |
| Custom native elements / 3D | B2/C/D proposed ([ADR-028](../decisions/ADR-028-custom-native-elements.md) accepted) |
| Text editing / forms | B2 implemented editing: grapheme-aware UTF-8 buffer, IME composition + preedit underline, caret/selection decoration, caret blink scheduling, bounded undo/redo with deterministic coalescing, and a HAL clipboard seam with cut/copy/paste (Win32 native, others documented); forms/validation model and bidi platform backends pending ([ADR-029](../decisions/ADR-029-interaction-completeness.md) accepted) |
| Scroll / gestures | B2 proposed ([ADR-029](../decisions/ADR-029-interaction-completeness.md) accepted) |
| Animation / overscroll | B2 runtime slice implemented; overscroll interaction design remains ([ADR-034](../decisions/ADR-034-interaction-and-overscroll-animation.md) accepted) |
| Overlays / layers | B2/C proposed ([ADR-029](../decisions/ADR-029-interaction-completeness.md) accepted) |
| Navigation / state restore | F proposed ([ADR-030](../decisions/ADR-030-app-platform-completeness.md) accepted) |
| Platform shell / i18n | F proposed ([ADR-030](../decisions/ADR-030-app-platform-completeness.md) accepted) |
| Accessibility | F partial: deterministic semantics snapshot (roles/names/states/actions from node metadata, hidden rules, focus) in `tessera_accessibility`; platform bridges (UIA/AT-SPI/NSAccessibility/Android) pending ([ADR-030](../decisions/ADR-030-app-platform-completeness.md) accepted) |
| Async data / offline | F proposed ([ADR-030](../decisions/ADR-030-app-platform-completeness.md) accepted) |
| Devtools / hot reload | C/D proposed ([ADR-031](../decisions/ADR-031-production-tooling-and-distribution.md) accepted) |
| Updates / signing / crash | F proposed ([ADR-031](../decisions/ADR-031-production-tooling-and-distribution.md) accepted) |
| Component library | after V1; proposed ([ADR-032](../decisions/ADR-032-bundled-component-library.md) accepted) |
| Samples / media / export | planned/deferred |
| Designer/Wasm | D proposed; hosted studio, data-driven skins, startup-phase preview and a `tessera-mcp` stdio adapter proposed with owner decisions recorded (`designer-studio-v1.md`, [ADR-045](../decisions/ADR-045-browser-hosted-studio.md)) |
| Packaging/release | F proposed |
| Pixel goldens on [lavapipe](reference/glossary.md)/software | B2 implemented: committed lavapipe (batched2d/dashboard/overlay/text/shapes) and CPU-canvas (rects/text/vector/image) baselines compared in CTest, plus the harness frame-capture baseline under `tests/golden/baseline/harness/`; the Linux `icu-text` job proves the opt-in ICU engine ([`golden-tests-v1.md`](specs/golden-tests-v1.md), [`testing.md`](guides/testing.md)) |
