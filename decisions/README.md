# Architecture Decision Records

Decisions are append-only. Superseding an ADR means writing a new one and marking the old one
`Superseded by ADR-XXX` (XXX is the number of the replacing ADR); do not rewrite history.

Status legend: **Accepted** (in force), **Proposed** (direction chosen, spike pending),
**Superseded** (replaced by a later ADR).
**Accepted (deferred)** means the decision is in force but the work is out of V1; example: [ADR-010](ADR-010-macos-via-moltenvk-deferred.md) (macOS through MoltenVK).
V1 is the first release. A feature is in V1 unless the ADR or spec that owns it says otherwise; see the [glossary](../docs/reference/glossary.md) entry V1 for the full definition.

| ADR | Title | Status |
|---|---|---|
| [ADR-001](ADR-001-vulkan-1-3-baseline.md) | Vulkan 1.3 as the native rendering baseline | Accepted |
| [ADR-002](ADR-002-webgpu-dawn-for-wasm.md) | WebGPU (Dawn) for design-time Wasm (WebAssembly), separate from Vulkan | Accepted |
| [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) | Thin RHI (render hardware interface) plus a batched 2D renderer | Accepted |
| [ADR-004](ADR-004-buy-vector-path-rasterization.md) | Buy vector path rasterization (ThorVG or Skia Graphite) | Accepted (backend spike pending) |
| [ADR-005](ADR-005-yoga-layout-adapter.md) | Yoga for Flexbox, with a small Grid subset | Accepted |
| [ADR-006](ADR-006-text-stack.md) | Text stack: FreeType, HarfBuzz, SheenBidi, ICU (International Components for Unicode) | Accepted |
| [ADR-007](ADR-007-compiler-ir.md) | A neutral IR (intermediate representation) between JSX and C++ codegen | Accepted |
| [ADR-008](ADR-008-protected-regions-and-manifest.md) | [Protected regions](../docs/reference/glossary.md), [binding manifest](../docs/reference/glossary.md), idempotent merging | Superseded by ADR-040 |
| [ADR-009](ADR-009-deterministic-headless-golden-tests.md) | Deterministic headless [golden](../docs/reference/glossary.md) tests on a software path | Accepted |
| [ADR-010](ADR-010-macos-via-moltenvk-deferred.md) | macOS through MoltenVK, deferred from V1 | Accepted (deferred) |
| [ADR-011](ADR-011-shader-authoring-and-cross-compilation.md) | Shader authoring and cross-compilation | Proposed |
| [ADR-012](ADR-012-static-linking.md) | Static linking as a hard requirement | Accepted |
| [ADR-013](ADR-013-project-license-mit.md) | Project license is MIT | Accepted |
| [ADR-014](ADR-014-project-config-format.md) | Project configuration is versioned `tessera.json` plus a C++ API | Accepted |
| [ADR-015](ADR-015-code-style-build-cache.md) | Code style and build-cache-friendly translation units | Accepted |
| [ADR-016](ADR-016-stylesheet-engine.md) | Stylesheet engine: full CSS syntax with staged semantics | Proposed |
| [ADR-017](ADR-017-input-and-fixed-step-runtime.md) | Input handler and fixed-step runtime loop | Proposed |
| [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) | Dirty rendering, [frame pacing](../docs/reference/glossary.md), and display refresh | Proposed |
| [ADR-019](ADR-019-power-aware-runtime.md) | Power-aware runtime defaults; no full-load game loop | Proposed |
| [ADR-020](ADR-020-window-resize-and-lifecycle.md) | Window resize, DPI (dots per inch, the display scale), and lifecycle separation | Proposed |
| [ADR-021](ADR-021-async-runtime.md) | Async/await runtime; never block the UI thread | Proposed |
| [ADR-022](ADR-022-diagnostics-and-profiling.md) | Structured diagnostics, tracing, and profiling | Proposed |
| [ADR-023](ADR-023-os-event-normalization-and-foldables.md) | OS event normalization and foldable metrics | Proposed |
| [ADR-024](ADR-024-native-first-boundaries.md) | Native-first boundaries; no production bridges | Accepted |
| [ADR-025](ADR-025-error-boundaries.md) | Error boundaries and exception isolation | Accepted |
| [ADR-026](ADR-026-deep-links-and-external-activation.md) | Deep links and external activation | Accepted |
| [ADR-027](ADR-027-agent-automation-and-audits.md) | Numeric IDs, headless harness, Lighthouse-style audits | Accepted |
| [ADR-028](ADR-028-custom-native-elements.md) | Custom native elements, low-level rendering, element input | Accepted |
| [ADR-029](ADR-029-interaction-completeness.md) | Text editing, scrolling/gestures, overlays, forms | Accepted |
| [ADR-030](ADR-030-app-platform-completeness.md) | Navigation, restore, shell, i18n (internationalization), accessibility, async data | Accepted |
| [ADR-031](ADR-031-production-tooling-and-distribution.md) | Devtools, updates/signing, crash reporting | Accepted |
| [ADR-032](ADR-032-bundled-component-library.md) | Bundled component library and design tokens | Accepted |
| [ADR-033](ADR-033-app-completeness-gap-resolutions.md) | App-completeness gap resolutions/deferrals | Accepted |
| [ADR-034](ADR-034-interaction-and-overscroll-animation.md) | Interaction animation and overscroll stretch/spring-back | Accepted |
| [ADR-035](ADR-035-view-transitions.md) | View transitions: global UI effects (theme circle reveal, glitch) | Accepted |
| [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) | Own JSX/TSX front end; React-like (not React-identical) model; built-in hooks; live view | Accepted |
| [ADR-037](ADR-037-self-contained-wasm-designer.md) | Self-contained Wasm designer engine (front end + evaluator), picker, style editing | Accepted |
| [ADR-038](ADR-038-cpp23-baseline.md) | C++23 language baseline (supersedes item 1 of ADR-015) | Accepted |
| [ADR-039](ADR-039-lowercase-html-tags-accepted.md) | Lowercase HTML tags accepted as aliases of intrinsics (reverses a subset-spec rule) | Accepted |
| [ADR-040](ADR-040-generated-base-and-developer-class.md) | Generated base class plus developer-owned class instead of protected regions (supersedes ADR-008) | Accepted |
| [ADR-041](ADR-041-layout-observation.md) | Layout observation (`onSizeChange`), a layout read model, and container queries second | Accepted |
| [ADR-042](ADR-042-jsx-startup-surface.md) | Declarative JSX startup surface: `App` root, built-in `Startup.*`/`Resources.*` member tags, framework loading screen | Proposed |
| [ADR-043](ADR-043-launch-modes-and-state-reuse.md) | Launch modes (cold/warm/hot resume/process restore) and state reuse, activation precedence, snapshot trust | Proposed |
| [ADR-044](ADR-044-system-window-chrome-in-v1.md) | Window chrome policy in V1: system chrome by default, Windows opt-in `WindowChrome` title bar, framework-owned `AppBar`, reserved chrome tokens | Accepted (revised) |
| [ADR-045](ADR-045-browser-hosted-studio.md) | Browser-hosted designer studio on GitHub Pages (page as host, HTML shell, data-driven skins) and a stdio MCP adapter of the same engine | Accepted |
| [ADR-046](ADR-046-web-target-for-demos.md) | Web (Wasm + WebGPU) as a secondary target for running apps in the browser (amends ADR-024 and ADR-002) | Accepted |
| [ADR-047](ADR-047-component-library-layers-and-design-language.md) | Component library layers, theme versioning, "Instrument" design language, catalog and stages (amends ADR-032) | Accepted |

## Dependency overview

```
ADR-001 Vulkan 1.3 ─┬─ ADR-003 RHI + 2D renderer ── ADR-004 vector paths
ADR-002 WebGPU/Dawn ─┘        │                     ADR-006 text
                              ├─ ADR-005 layout
                              ├─ ADR-009 goldens
                              └─ ADR-011 shaders

ADR-007 IR ── ADR-008 protected regions (superseded) ── ADR-040 generated base + developer class
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
ADR-041 layout observation (accepted) ── ADR-020 resize, ADR-018 dirty rendering, ADR-016 stylesheet
ADR-046 web target for demos ── ADR-002 WebGPU, ADR-024 boundaries, ADR-037 designer
ADR-047 component library layers/design language ── ADR-032 component library, ADR-016 stylesheet
```

[ADR-035](ADR-035-view-transitions.md), [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md), and [ADR-037](ADR-037-self-contained-wasm-designer.md) are listed in the table above but are not drawn in this diagram.

## Adding an ADR

1. Copy the structure of an existing ADR: Status, Date, Related, Context, Decision,
   Consequences, Alternatives, Follow-up.
2. Use the next number after the highest ADR in the table above and a slug, in the form `ADR-NNN-short-title.md` (NNN is the three-digit number).
3. Update the table above.
4. If it supersedes an ADR, update the old file's status and link the replacement.
