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
| [ADR-008](ADR-008-protected-regions-and-manifest.md) | Protected regions, binding manifest, idempotent merging (superseded; markers and merger removed) | Superseded by ADR-040 |
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
| [ADR-023](ADR-023-os-event-normalization-and-foldables.md) | OS event normalization and foldable metrics | Accepted |
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
| [ADR-039](ADR-039-lowercase-html-tags-accepted.md) | Lowercase HTML tags accepted as aliases of intrinsics (reverses a subset-spec rule). Decisions 3 and 4 amended by ADR-059 | Accepted (amended by [ADR-059](ADR-059-html-tags-supported-not-rejected.md)) |
| [ADR-040](ADR-040-generated-base-and-developer-class.md) | Generated base class plus developer-owned class instead of protected regions (supersedes ADR-008); the only codegen path since #219 | Accepted (migration complete 2026-11-15) |
| [ADR-041](ADR-041-layout-observation.md) | Layout observation (`onSizeChange`), a layout read model, and container queries second | Accepted |
| [ADR-042](ADR-042-jsx-startup-surface.md) | Declarative JSX startup surface: `App` root, built-in `Startup.*`/`Resources.*` member tags, framework loading screen | Proposed |
| [ADR-043](ADR-043-launch-modes-and-state-reuse.md) | Launch modes (cold/warm/hot resume/process restore) and state reuse, activation precedence, snapshot trust | Proposed |
| [ADR-044](ADR-044-system-window-chrome-in-v1.md) | Window chrome policy in V1: system chrome by default, Windows opt-in `WindowChrome` title bar, framework-owned `AppBar`, reserved chrome tokens | Accepted (revised) |
| [ADR-045](ADR-045-browser-hosted-studio.md) | Browser-hosted designer studio on GitHub Pages (page as host, HTML shell, data-driven skins) and a stdio MCP adapter of the same engine | Accepted |
| [ADR-046](ADR-046-web-target-for-demos.md) | Web (Wasm + WebGPU) as a secondary target for running apps in the browser (amends ADR-024 and ADR-002) | Accepted |
| [ADR-047](ADR-047-component-library-layers-and-design-language.md) | Component library layers, theme versioning, "Instrument" design language, catalog and stages (amends ADR-032) | Accepted |
| [ADR-048](ADR-048-precompiled-embedded-stylesheets.md) | Precompiled stylesheets embedded as versioned blobs (prepare/resolve split), import-graph cascade order, library CSS in `@layer` | Accepted |
| [ADR-049](ADR-049-backend-extensibility.md) | Backend extensibility: RHI versioning, `shader_id` and shader sources for non-Vulkan backends, conformance suite, support tiers, community backends (amends ADR-011) | Accepted |
| [ADR-050](ADR-050/README.md) | Application-framework layer above the UI runtime: app lifecycle, services with compile-time ids resolved once at mount (no RTTI), TSX `useService<Interface>()`, task scopes, timers, observable app state, native render surfaces via typed refs (a folder of ten parts; the former `ADR-050-opt-in-app-framework-layer.md` reached the 600-line limit) | Accepted |
| [ADR-051](ADR-051-ir-record-types-and-record-values.md) | IR record types and record values: an optional module type table and a `record` expression, additive to IR v1; unblocks object-literal prop defaults (#158). Accepted (2026-11-13, owner); slices 3-4 landed, with a per-module `<module>_types.hpp` replacing the "first using component" placement (2026-11-15) |
| [ADR-052](ADR-052-networking-platform-stacks.md) | Networking: platform HTTP stacks (WinHTTP, NSURLSession, `fetch`, libcurl on Linux only) behind one `fetch`-shaped interface with a contract suite; WebSocket after V1. Accepted (2026-11-13, owner) |
| [ADR-053](ADR-053-native-metal-backend.md) | Native Metal backend for macOS and iOS (MSL through the ADR-049 shader path, Tier 2 first); supersedes ADR-010 items 2, 3 and 5. Accepted (2026-11-13, owner) |
| [ADR-054](ADR-054-studio-front-end-react-vite.md) | Studio front end is a React 19 + Vite application over the static Wasm host page; chrome only, design-time mock services are host TypeScript modules (amends the page-stack wording of ADR-045, which is not superseded). Accepted (2026-11-16, owner) |
| [ADR-055](ADR-055-studio-mock-services.md) | Design-time mock services are `*.mock.ts` project files compiled and run in a sandboxed studio worker; the engine never executes TypeScript; narrows ADR-045 decision 7 for mock files only. Accepted (2026-11-16, owner chose option B); implementation gated on the service seam |
| [ADR-056](ADR-056-update-dependency-masks-and-scheduled-children.md) | Generated components re-evaluate only bindings whose compile-time dependency bits are dirty, and a parent schedules a child's ticket on a prop change instead of calling its `update()`; answers the #440 fan-out measurement. Accepted (2026-11-17); slices #441, #442, #443 |
| [ADR-057](ADR-057-vector-paths-thorvg-default-and-exit.md) | `TESSERA_ENABLE_THORVG` defaults ON (the Linux lane already proves it; the Windows lane pins it OFF for its build budget), and ThorVG is replaced by an in-house rasterizer behind the unchanged `PathRasterizer` seam when a measured trigger fires. Amends ADR-004 decision 1. Accepted (2026-11-18, owner) |
| [ADR-058](ADR-058-react-vite-port-compatibility-tiers.md) | Porting a React + Vite project: every dependency and construct falls in one of four classes (source-compatible, same-API native shim package, build-time translation, not ported with a named replacement); a `port-check` tool reports the class; a typing budget says what a port edits (declarations) and what the compiler absorbs (`interface`, `enum`, `?.`, `??`, `number` inference); no npm source is ever compiled. Proposed (2026-11-19); five questions for the owner |
| [ADR-059](ADR-059-html-tags-supported-not-rejected.md) | HTML tags get one of four statuses (mapped, approximated with a warning, planned with an issue, rejected only where a web engine or JS runtime is needed) instead of being rejected; carriers stay `View`/`Text` with meaning in role, a reserved class and props; an opt-in `ui.htmlDefaults: "browser"` user-agent stylesheet resolves Q12; capability groups A-F are the roadmap. Amends ADR-039 decisions 3 and 4. Accepted (2026-11-19, owner): browser defaults on by default, full table layout, both link forms, `approximated` is a warning (error under `ui.strict`) |
| [ADR-060](ADR-060-studio-services-in-web-workers.md) | Studio services (viewer, analysis/language, format, mock) are Wasm modules in Web Workers behind one UI-agnostic, versioned, cancellable message protocol, so the V1 Tessera-built studio reuses them; parallelism is several workers, not pthreads (no SharedArrayBuffer on the static host); one C++ analysis library for the worker, CLI and MCP; Chromium only for now. Accepted (2026-11-19, owner): C++ front end in Wasm, the analysis pool built from the start, non-Chromium browsers get the read-only viewer |
| [ADR-061](ADR-061-compiler-owned-formatter.md) | The compiler owns formatting: one canonical printer (`tessera fmt`) for `.tsx`, `.css` and `tessera.json`, idempotent, meaning-preserving, never drops a comment, returns a minimal edit list; not Prettier. Accepted (2026-11-19, owner): the proposed style, format-on-save on by default, an unplaceable comment refuses the file |
| [ADR-062](ADR-062-studio-dock-library-flexlayout-react.md) | The studio dock is flexlayout-react 0.11.2 (exact-pinned) behind our own canonical layout JSON, operations and a thin adapter; no floating windows in V1; proven in a real browser with a real WebGPU canvas. Accepted (2026-11-19) |
| [ADR-063](ADR-063-browser-matching-user-agent-defaults.md) | Default styles reproduce the browser's look as far as possible, exceptions named (supersedes react-compat decision 12 and the UA-parity non-goal): one shared user-agent sheet (`ua_stylesheet_text()`) in every cascade (app host and preview) at `Origin::UserAgent`, below author/theme/inline; grows one tested rule at a time, first rule centres a bare `Button`'s label (#602); samples ship real author CSS to show the full stack. Accepted (2026-11-20, owner) |
| [ADR-064](ADR-064-astryx-style-component-system.md) | First-party UI library follows Astryx's system design (catalogue, anatomy, role-token theme model, behavior hooks, plugin tables with caller-owned server data); Instrument becomes a demo theme (amends ADR-047 item 5) | Proposed |
| [ADR-065](ADR-065-custom-hooks-by-compile-time-inlining.md) | Custom hooks are accepted in V1 by compile-time inlining at each call site (amends ADR-036 item 3; answers ADR-058 question 3 for custom hooks) | Proposed |
| [ADR-066](ADR-066-react-core-additions-for-v1.md) | React core additions for V1: arrow components, `import React`, `memo`, `useId`, `useMemo`/`useCallback`, `??`/`?.`, handler control flow, list and string expressions, limited `useReducer` and static `useContext` (amends ADR-036) | Proposed |
| [ADR-067](ADR-067-pending-ui-transitions-suspense-lazy.md) | Pending UI: `useResource`, `Suspense` as a static boundary, `useTransition`, `useDeferredValue`, `lazy` as identity, on the data store's resource model | Proposed |
| [ADR-068](ADR-068-clip-state-in-a-storage-buffer.md) | Clip state in a per-frame storage buffer: RHI 1.12 `bind_storage_buffer`, clip records for rounded rect, ellipse and polygon, `CpuCanvas` shaped-clip reference, for `clip-path` (#615) | Accepted |
| [ADR-069](ADR-069-awaitable-declares-no-resume.md) | An awaitable declares when its frame will never resume: `declare_no_resume` drops the continuation reference so an abandoned task pair is freed at its owner instead of leaking (#770) | Accepted |
| [ADR-070](ADR-070-frame-arena-not-pmr.md) | A custom frame arena, not `std::pmr`: one block with an explicit reset, a high-water counter and a fixed capacity that fails with a diagnostic; closes memory-model-v1 open question 1 (#701) | Accepted |
| [ADR-071](ADR-071-gpu-compute-tile-rasterizer.md) | A GPU compute tile rasterizer (Vello-style) for vector paths, amending ADR-057/ADR-004: cost scales with segments, not pixels; encode stays CPU-incremental, flatten/stroke/morph on the GPU; evidence for the spike is still open (#683) | Proposed |
| [ADR-072](ADR-072-rhi-copy-buffer-to-texture.md) | RHI 1.17: copy a buffer into a texture inside a command list (the handoff from the compute rasterizer to the renderer), one command and one capability; alternatives include a resolve pass that needs no RHI change (#683) | Proposed |

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
ADR-048 precompiled stylesheets (accepted) ── ADR-016 stylesheet, ADR-047 component library, ADR-046 web target
ADR-050 app framework layer ── ADR-021 async, ADR-030 app platform, ADR-040 generated base, ADR-043 launch modes
ADR-051 IR record types (accepted) ── ir-v1 spec, jsx-subset-v1 8.5, ADR-040 generated base
ADR-052 networking (accepted) ── ADR-050 app framework, ADR-021 async, ADR-012 static linking
ADR-053 native Metal backend (accepted) ── ADR-010 macOS deferred (superseded in part), ADR-049 backend extensibility, ADR-003 thin RHI
ADR-054 studio front end React 19 + Vite (accepted) ── ADR-045 browser-hosted studio (page stack amended), ADR-024 boundaries (unchanged), ADR-050 section 3.1 item 6 (preview fakes)
ADR-055 studio mock services (accepted) ── ADR-054 studio stack, ADR-045 decision 7 (narrowed for mock files), ADR-050 preview fakes, services design (post-V1)
ADR-056 update dependency masks (accepted) ── ADR-040 generated base, ADR-018 dirty rendering, observable design note
ADR-057 vector paths: ThorVG default ON + measured exit (accepted) ── ADR-004 buy vector paths (decision 1 amended), ADR-012 static linking, ADR-009 goldens
ADR-058 React + Vite port compatibility classes (proposed) ── ADR-036 own front end, ADR-039 lowercase tags, ADR-024 no JS runtime, ADR-051 record types, ADR-050 app framework
ADR-059 HTML tags supported not rejected (accepted) ── ADR-039 lowercase tags (amended), ADR-058 port classes, ADR-036 closed IR kinds, ADR-024 no JS runtime
ADR-060 studio services in web workers (accepted) ── ADR-045 browser-hosted studio, ADR-054 React chrome (stepping stone), ADR-055 mock worker, ADR-024 no JS runtime, ADR-061 formatter service
ADR-061 compiler-owned formatter (accepted) ── ADR-036 own front end, ADR-060 format worker, ADR-058 ports, ADR-024 no JS runtime
ADR-062 studio dock library (accepted) ── ADR-054 React renders the canvas once, ADR-060 V1 rebuilt with Tessera, workbench layout spec
ADR-064 Astryx-style component system (proposed) ── ADR-047 component library, ADR-063 UA defaults, ADR-016 stylesheet, ADR-024 no JS runtime, ADR-034 interaction
ADR-065 custom hooks by inlining (proposed) ── ADR-036 own front end, ADR-058 React ports, ADR-064 behavior hooks, ADR-040 generated base
ADR-066 React core additions (proposed) ── ADR-036 own front end, ADR-058 React ports, ADR-065 custom hooks, ADR-051 records
ADR-067 pending UI (proposed) ── async-data-v1 resources, ADR-021 async runtime, ADR-050 services, ADR-066 React additions, error-boundaries-v1
ADR-068 clip storage buffer (accepted) ── ADR-003 thin RHI, ADR-049 RHI versioning, ADR-053 Metal, ADR-004 vector paths
ADR-069 awaitable declares no resume (accepted) ── ADR-021 async runtime, ADR-050 app framework, async design, background-work design
ADR-070 frame arena (accepted) ── memory-model-v1 sections 2/3/9, #701 measurement
ADR-071 GPU compute tile rasterizer (proposed) ── ADR-057 vector paths exit, ADR-004, ADR-068 storage buffers, ADR-049 RHI versioning
ADR-072 RHI copy buffer to texture (proposed) ── ADR-071 compute rasterizer handoff, ADR-049 RHI versioning, ADR-003 thin RHI
```

[ADR-035](ADR-035-view-transitions.md), [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md), and [ADR-037](ADR-037-self-contained-wasm-designer.md) are listed in the table above but are not drawn in this diagram.

## Adding an ADR

1. Copy the structure of an existing ADR: Status, Date, Related, Context, Decision,
   Consequences, Alternatives, Follow-up.
2. Use the next number after the highest ADR in the table above and a slug, in the form `ADR-NNN-short-title.md` (NNN is the three-digit number). An ADR that would exceed the 600-line docs hard limit is a folder instead: `ADR-NNN-short-title/README.md` keeps the title, `- Status:`, `- Date:` and `- Related:` front matter plus a `## Parts` table, and the section bodies move verbatim into the linked `*.md` parts, keeping their heading numbers (see [ADR-050](ADR-050/README.md)).
3. Update the table above. For a folder ADR, link its `README.md`.
4. If it supersedes an ADR, update the old file's status and link the replacement.
