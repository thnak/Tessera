# Architecture Decision Records

Decisions are append-only. Superseding an ADR means writing a new one and marking the old one
`Superseded by ADR-XXX`; do not rewrite history.

Status legend: **Accepted** (in force), **Proposed** (direction chosen, spike pending),
**Superseded** (replaced by a later ADR).

| ADR | Title | Status |
|---|---|---|
| [ADR-001](ADR-001-vulkan-1-3-baseline.md) | Vulkan 1.3 as the native rendering baseline | Accepted |
| [ADR-002](ADR-002-webgpu-dawn-for-wasm.md) | WebGPU (Dawn) for design-time Wasm, separate from Vulkan | Accepted |
| [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) | Thin RHI plus a batched 2D renderer | Accepted |
| [ADR-004](ADR-004-buy-vector-path-rasterization.md) | Buy vector path rasterization (ThorVG or Skia Graphite) | Accepted (backend spike pending) |
| [ADR-005](ADR-005-yoga-layout-adapter.md) | Yoga for Flexbox, with a small Grid subset | Accepted |
| [ADR-006](ADR-006-text-stack.md) | Text stack: FreeType, HarfBuzz, SheenBidi, ICU | Accepted |
| [ADR-007](ADR-007-compiler-ir.md) | A neutral IR between JSX and C++ codegen | Accepted |
| [ADR-008](ADR-008-protected-regions-and-manifest.md) | Protected regions, binding manifest, idempotent merging | Accepted |
| [ADR-009](ADR-009-deterministic-headless-golden-tests.md) | Deterministic headless golden tests on a software path | Accepted |
| [ADR-010](ADR-010-macos-via-moltenvk-deferred.md) | macOS through MoltenVK, deferred from V1 | Accepted (deferred) |
| [ADR-011](ADR-011-shader-authoring-and-cross-compilation.md) | Shader authoring and cross-compilation | Proposed |
| [ADR-012](ADR-012-static-linking.md) | Static linking as a hard requirement | Accepted |
| [ADR-013](ADR-013-project-license-mit.md) | Project license is MIT | Accepted |
| [ADR-014](ADR-014-project-config-format.md) | Project configuration is versioned `tessera.json` plus a C++ API | Accepted |
| [ADR-015](ADR-015-code-style-build-cache.md) | Code style and build-cache-friendly translation units | Accepted |
| [ADR-016](ADR-016-stylesheet-engine.md) | Stylesheet engine: full CSS syntax with staged semantics | Proposed |
| [ADR-017](ADR-017-input-and-fixed-step-runtime.md) | Input handler and fixed-step runtime loop | Proposed |
| [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) | Dirty rendering, frame pacing, and display refresh | Proposed |
| [ADR-019](ADR-019-power-aware-runtime.md) | Power-aware runtime defaults; no full-load game loop | Proposed |
| [ADR-020](ADR-020-window-resize-and-lifecycle.md) | Window resize, DPI, and lifecycle separation | Proposed |
| [ADR-021](ADR-021-async-runtime.md) | Async/await runtime; never block the UI thread | Proposed |
| [ADR-022](ADR-022-diagnostics-and-profiling.md) | Structured diagnostics, tracing, and profiling | Proposed |
| [ADR-023](ADR-023-os-event-normalization-and-foldables.md) | OS event normalization and foldable metrics | Proposed |
| [ADR-024](ADR-024-native-first-boundaries.md) | Native-first boundaries; no production bridges | Accepted |
| [ADR-025](ADR-025-error-boundaries.md) | Error boundaries and exception isolation | Accepted |
| [ADR-026](ADR-026-deep-links-and-external-activation.md) | Deep links and external activation | Accepted |
| [ADR-027](ADR-027-agent-automation-and-audits.md) | Numeric IDs, headless harness, Lighthouse-style audits | Accepted |
| [ADR-028](ADR-028-custom-native-elements.md) | Custom native elements, low-level rendering, element input | Accepted |
| [ADR-029](ADR-029-interaction-completeness.md) | Text editing, scrolling/gestures, overlays, forms | Accepted |
| [ADR-030](ADR-030-app-platform-completeness.md) | Navigation, restore, shell, i18n, accessibility, async data | Accepted |
| [ADR-031](ADR-031-production-tooling-and-distribution.md) | Devtools, updates/signing, crash reporting | Accepted |
| [ADR-032](ADR-032-bundled-component-library.md) | Bundled component library and design tokens | Accepted |
| [ADR-033](ADR-033-app-completeness-gap-resolutions.md) | App-completeness gap resolutions/deferrals | Accepted |
| [ADR-034](ADR-034-interaction-and-overscroll-animation.md) | Interaction animation and overscroll stretch/spring-back | Accepted |

## Dependency overview

```
ADR-001 Vulkan 1.3 ─┬─ ADR-003 RHI + 2D renderer ── ADR-004 vector paths
ADR-002 WebGPU/Dawn ─┘        │                     ADR-006 text
                              ├─ ADR-005 layout
                              ├─ ADR-009 goldens
                              └─ ADR-011 shaders

ADR-007 IR ── ADR-008 protected regions
ADR-010 macOS (deferred)
ADR-012 static linking (cross-cutting: build, licenses, accessibility)
ADR-013 MIT license (cross-cutting: dependency choices)
ADR-014 project config (cross-cutting: generator, runtime, tooling)
ADR-015 code style/build cache (cross-cutting: all C++ code)
ADR-016 stylesheet engine ── ADR-005 layout
ADR-017 input + fixed-step loop ── ADR-018 dirty rendering/frame pacing
ADR-019 power-aware defaults (cross-cutting: runtime, pacing, quality)
ADR-020 window resize/lifecycle ── ADR-017 loop, ADR-021 async
ADR-021 async/await runtime ── ADR-020 resize, ADR-022 diagnostics
ADR-022 diagnostics/profiling (cross-cutting: all subsystems)
ADR-023 OS event normalization/foldables ── ADR-020 lifecycle, ADR-017 input
ADR-024 native-first boundaries (cross-cutting: runtime, compiler, designer, mobile)
ADR-025 error boundaries (supersedes the error-boundary limit in ADR-007)
ADR-026 deep links/external activation (cross-cutting: HAL, routing, security, startup)
ADR-027 automation IDs/headless/audits (cross-cutting: compiler, UI, diagnostics, CI)
ADR-028 custom native elements (cross-cutting: JSX/IR, RHI, shaders, input, tests)
ADR-029 interaction completeness (text editing, scroll/gestures, overlays, forms)
ADR-030 app platform (navigation, restore, shell, i18n, accessibility, async data)
ADR-031 production tooling (devtools/hot reload, updates, crash reporting)
ADR-032 component library/design tokens (product layer)
ADR-033 gap resolutions (cross-cutting: text/scroll/overlay/forms/nav/restore/shell/i18n/a11y/data/production)
ADR-034 interaction/overscroll animation (visual state, platform profiles, spring-back)
```

## Adding an ADR

1. Copy the structure of an existing ADR: Status, Date, Related, Context, Decision,
   Consequences, Alternatives, Follow-up.
2. Use the next number and a slug: `ADR-012-short-title.md`.
3. Update the table above.
4. If it supersedes an ADR, update the old file's status and link the replacement.
