# Tessera

> Working name — easy to rename.

[![Windows](https://github.com/thnak/Tessera/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/thnak/Tessera/actions/workflows/windows.yml)
[![Linux](https://github.com/thnak/Tessera/actions/workflows/linux.yml/badge.svg?branch=main)](https://github.com/thnak/Tessera/actions/workflows/linux.yml)
[![WebGPU](https://github.com/thnak/Tessera/actions/workflows/webgpu.yml/badge.svg?branch=main)](https://github.com/thnak/Tessera/actions/workflows/webgpu.yml)

Windows, Linux and WebGPU are the gating CI lanes. macOS, Android and iOS run nightly and are
advisory ([V1 platform scope](AGENTS.md#v1-platform-scope-and-ci-policy)).

A high-performance C++ UI framework. You write **JSX/React syntax**, a compiler turns it into
**native C++**, and the app is drawn by Tessera's own renderer: **Vulkan 1.3** natively, **WebGPU
(Wasm)** for an emulator-free preview in the browser. Rendering is deterministic and headless-capable,
so an AI agent or a CI job can diff screenshots. Production apps carry no JavaScript or React runtime.

## Status

**Pre-alpha.** Not usable to build an app yet; the pieces below are landing in the order set by
the [V1 milestones](AGENTS.md#big-steps-group-work-dont-micro-step).

| Area | State on `main` |
|---|---|
| Renderer | Done: thin RHI (render hardware interface), null and Vulkan 1.3 backends, batched 2D (solid, SDF, textured quads, clip and transform stacks), lavapipe and CPU pixel goldens. WebGPU backend: first slice, unbuilt on the host |
| Runtime | Done: fixed-step loop, frame pacing, power-aware idle (a static screen makes no presents), dirty rendering and damage rects, async executors and tasks, adaptive quality, structured diagnostics |
| UI engine | Done: scene graph and W3C-style events, stylesheet engine (layers, media, `@supports`, `var()`), Yoga flexbox, text stack (FreeType, HarfBuzz, SheenBidi), transitions, keyframes and springs, virtualized lists |
| Windowing | Win32 and X11 windows and swapchains, device-loss recovery. UIA and AT-SPI bridges exist but are not yet installed in a host. Wayland not yet |
| Compiler | TSX front end, IR, C++ codegen with a generated `Base<Self>` plus a developer class (ADR-040), forms, lists, animated values. Migration of the remaining codegen test families is in progress |
| Design time | Specified, not built. `designer/web` and `designer/native` are placeholders |
| Samples | `click-demo`, `compiler-demo`, `parity-demo`; the `hello` and `dashboard` samples are empty |

The per-item checklist with evidence paths is in [AGENTS.md](AGENTS.md); the history of what landed is in
[`docs/architecture/implementation-log.md`](docs/architecture/implementation-log.md). Terms are defined in the
[glossary](docs/reference/glossary.md).

## What V1 will ship

V1 targets **Windows and Linux** (owner decision, 2026-11-13). Scope and acceptance criteria live in the
eight [GitHub milestones](https://github.com/thnak/Tessera/milestones); this is the promise in plain words.

| # | Milestone | What you get |
|---|---|---|
| M1 | Core runtime | Stylesheet engine, layout, text, input, loop and pacing, async, diagnostics, goldens, window and swapchain, OS events. A static dashboard idles with zero presents; goldens are green on Windows and Linux |
| M2 | Compiler and animation | JSX/TSX to native C++ with `useState`, `useEffect`, `useRef` and animated values that keep your hand-written C++ across regeneration; CSS animation and transition events |
| M3 | Backends and shader chain | Versioned RHI, `shader_id` registry, one GLSL source for every backend, a backend conformance suite, support tiers, a guide to writing a backend, and `backdrop-filter` glass ([`glass-v1.md`](docs/specs/glass-v1.md)) |
| M4 | Design-time preview | The same engine in the browser (see Design time below) |
| M5 | Application framework | `Application` and window lifecycle, services (`useService<T>()`), task scopes and timers, `Observable<T>`, native render surfaces, storage, multiple windows, tray apps, and the first bundled components (library stages 0 and 1) |
| M6 | Data-heavy UI | Zero-allocation chart series, retained/immediate hybrid nodes with damage rects, frame-budget metrics, a streaming dashboard under sustained load |
| M7 | Platform breadth and accessibility | Windows and Linux complete: window, input, IME, lifecycle, full-screen, cursor, translucent windows (Mica, Acrylic, compositor blur); UIA and AT-SPI accessibility |
| M8 | Release readiness | Packaging, updates, crash capture with symbolication, security review, versioning policy for RHI, IR and project config, `i18n`, navigation and networking modules, release CI lanes |

V1 acceptance: a real app ships and passes validation layers and golden tests in CI.

### Bundled component library (`tessera-ui`)

Source plus generated C++ built only on public APIs ([ADR-032](decisions/ADR-032-bundled-component-library.md)).
V1 ships **stage 0** (behavior primitives, the Instrument design tokens with light, dark and high-contrast
themes, bundled fonts, `tessera icons add`) and **stage 1** (core controls, inputs, layout, feedback,
`SafeArea`, `Image`, `List`, `Tooltip`; enough for a settings screen). Every component documents its role,
keyboard and focus behavior, reduced motion and high contrast.

### Not in V1

Specified and kept for later: macOS (native Metal, [ADR-053](decisions/ADR-053-native-metal-backend.md)),
Android, iOS, fold and posture support, and library stages 2 to 6 (element transitions, `LiveCapsule`,
media, map, PDF viewer; see the [documentation map](docs/README.md)).

## Toolkit

What a developer will use, and where each piece stands.

| Tool | Purpose | State |
|---|---|---|
| `tessera_codegen_cli` | TSX to C++ with the generated base class and a developer class | Exists (`src/compiler/codegen`) |
| `tessera` CLI: `new`, `dev`, `build`, `run` | Project templates, a prebuilt toolchain, a five-minute first app ([#77](https://github.com/thnak/Tessera/issues/77)) | Planned for V1; described in [getting started](docs/guides/getting-started.md) |
| `tessera icons add` | Import Iconify icons through a license gate ([#238](https://github.com/thnak/Tessera/issues/238)) | Planned for V1 |
| Theme generator | Instrument token sets from one seed color with enforced WCAG contrast ([#79](https://github.com/thnak/Tessera/issues/79)) | Planned for V1 |
| Headless harness | Deterministic offscreen frames, automation ids, golden diffs for agents ([guide](docs/guides/agent-automation.md)) | Exists |
| `tessera-mcp` | stdio MCP adapter over the harness session ([#172](https://github.com/thnak/Tessera/issues/172)) | Planned for V1 |
| Devtools and hot reload | `#line` mapping to TSX, state-keeping hot reload, in-app error overlay ([#78](https://github.com/thnak/Tessera/issues/78)) | Planned for V1 |
| Public API listings | Committed header and component listings with a CI gate ([#76](https://github.com/thnak/Tessera/issues/76)) | Planned for V1 |
| CPU benchmark | A GPU-free 12,000-node benchmark with a committed budget ([#241](https://github.com/thnak/Tessera/issues/241)) | Planned for V1 |
| Wiki client | Static client for the GitHub Wiki guides (`tools/wiki`; Node is design-time only) | Exists |

## Design time

The designer runs the **real C++ engine** in a browser through Emscripten and WebGPU. It is not an emulator
and not a second renderer ([design](docs/design/designer.md)). What V1 promises, in build order
(issues #164 to #171):

1. The compiler in Wasm, and a diagnostics-only page on GitHub Pages.
2. An IR evaluator in the browser, so hooks and handlers run without a native build.
3. A WebGPU canvas preview with browser input; component state survives edits.
4. Shareable links, a sample gallery, and theme, DPR and viewport toggles.
5. Size and latency budgets enforced in CI.
6. Studio slices: a picker overlay, a style inspector that writes a `source.patch`, and device skins.

Acceptance: the same screen previews in a browser and matches native output within the golden tolerance.
Native hot reload, the in-app error overlay and `tessera-mcp` complete the loop for agents.

Contributions: see [CONTRIBUTING.md](CONTRIBUTING.md) for setup, build/test, code style, documentation rules, and the PR checklist.

## Documents

Start with the [documentation map](docs/README.md) and [architecture overview](docs/architecture/overview.md).

- [Architecture overview](docs/architecture/overview.md)
- [Module map](docs/architecture/module-map.md)
- [Getting started](docs/guides/getting-started.md)
- [Design plan v1](../ui_framework_design_plan_v1.md) (file in the parent directory of this repository, not tracked here)
- [Execution plan & workload breakdown](../ui_framework_execution_plan.md) (file in the parent directory of this repository, not tracked here)
- Architecture decisions: [`decisions/`](decisions/) — index at
  [`decisions/README.md`](decisions/README.md)
- [Project generator & developer config](docs/guides/project-generator.md)
- [Developer workflow & protected regions](docs/guides/developer-workflow.md)
- [`tessera.json` schema v1](docs/specs/project-config-v1.md)
- [Code style & build cache](docs/guides/code-style.md)
- [Dependency map](docs/specs/dependencies.md)
- [Renderer / RHI design](docs/design/renderer.md), [GPU docs](docs/design/gpu/README.md),
  [anti-aliasing](docs/design/antialiasing.md)
- [Scene graph & events](docs/design/scene-graph.md)
- [UI model](docs/design/ui-model.md)
- [Layout / text / animation](docs/design/layout.md), [text](docs/design/text.md),
  [animation](docs/design/animation.md), [animation runtime](docs/design/animation-runtime.md)
- [Responsive / accessibility](docs/design/responsive.md), [accessibility](docs/design/accessibility.md)
- [Stylesheet engine spec](docs/specs/stylesheet-v1.md)
- [Compiler / IR / component model](docs/design/compiler.md), [IR spec](docs/specs/ir-v1.md),
  [component model](docs/specs/component-model-v1.md)
- [UI virtualization](docs/specs/virtualization-v1.md)
- [Design-time viewer](docs/design/designer.md)
- [Runtime config spec](docs/specs/runtime-config-v1.md)
- [Runtime loop & frame pacing](docs/design/game-loop.md)
- [Dirty rendering & invalidation](docs/design/dirty-rendering.md)
- [Performance & power](docs/design/performance.md)
- [Window resize & lifecycle](docs/design/window-resize.md)
- [HAL](docs/design/hal.md), [OS events & foldables](docs/specs/os-events-v1.md)
- [Mobile integration](docs/design/mobile-integration.md)
- [Memory model](docs/specs/memory-model-v1.md), [buffer config & tunables](docs/specs/buffer-config-v1.md),
  [tunables guide](docs/guides/tunables.md), [bridges](docs/design/bridges.md)
- [Startup & loading](docs/design/startup-and-loading.md), [resource lifecycle](docs/specs/resource-lifecycle-v1.md),
  [loading matrix](docs/guides/loading-matrix.md), [error boundaries](docs/specs/error-boundaries-v1.md),
  [deep linking](docs/specs/deep-linking-v1.md), [deep-linking guide](docs/guides/deep-linking.md)
- [Automation IDs](docs/specs/automation-ids-v1.md), [headless automation](docs/specs/headless-automation-v1.md),
  [agent guide](docs/guides/agent-automation.md), [Lighthouse audits](docs/specs/lighthouse-audit-v1.md)
- [Custom native elements](docs/specs/custom-elements-v1.md), [custom elements guide](docs/guides/custom-elements.md)
- [Text editing](docs/specs/text-editing-v1.md), [scroll/gestures](docs/specs/scroll-and-gestures-v1.md),
  [animation/overscroll](docs/specs/animation-v1.md), [view transitions](docs/specs/view-transitions-v1.md), [overlays/layers](docs/specs/overlays-and-layers-v1.md),
  [forms/validation](docs/specs/forms-and-validation-v1.md)
- [Navigation](docs/specs/navigation-v1.md), [state restoration](docs/specs/state-restoration-v1.md),
  [platform shell](docs/specs/platform-shell-v1.md), [i18n](docs/specs/i18n-v1.md),
  [accessibility](docs/specs/accessibility-v1.md), [async data](docs/specs/async-data-v1.md)
- [Devtools](docs/specs/devtools-protocol-v1.md), [updates/distribution](docs/specs/updates-and-distribution-v1.md),
  [crash reporting](docs/specs/crash-reporting-v1.md), [component library](docs/design/component-library.md),
  [sample apps](docs/guides/sample-apps.md), [media/export](docs/design/media-and-export.md)
- [App-completeness cross-check](docs/architecture/app-completeness-cross-check-v1.md) —
  ownership, conflicts, gap register, consolidated open questions
- [Gap resolutions](docs/architecture/app-completeness-gap-resolutions-v1.md) —
  resolved/deferred decisions ([ADR-033](decisions/ADR-033-app-completeness-gap-resolutions.md))
- [Async runtime](docs/design/async.md)
- [Threading & memory](docs/design/threading-and-memory.md)
- [Resources](docs/design/resources.md), [services](docs/design/services.md),
  [security](docs/design/security.md)
- [Diagnostics & profiling](docs/design/diagnostics.md)
- [Diagnostics config](docs/specs/diagnostics-config-v1.md)
- [Input handler](docs/design/input.md)
- [Testing](docs/guides/testing.md), [golden tests](docs/specs/golden-tests-v1.md)
- [Packaging](docs/guides/packaging.md), [build & packaging design](docs/design/build-and-packaging.md)
- [Debugging](docs/guides/debugging.md), [glossary](docs/reference/glossary.md)
- [Batched2D reference image](docs/images/batched2d.png)
- Design / specs / guides: [`docs/`](docs/)

## Repository layout

```text
Tessera/
├── docs/                 design notes, specs, RFCs, guides, research
├── decisions/            ADRs (ADR-001 ...)
├── include/tessera/      public headers
├── src/
│   ├── core/             object, memory, threading, event, resource
│   ├── hal/              window, display, input, graphics, audio,
│   │                     filesystem, power, timer, lifecycle
│   ├── ui/               view, scene, layout, style, animation, responsive,
│   │                     text, accessibility
│   ├── rendering/        rhi, gpu, scenegraph, scheduler, resources, null,
│   │                     backend/{vulkan,webgpu}, software
│   ├── services/         i18n, navigation, notification, storage, networking
│   ├── diagnostics/      logging, profiling, crash
│   ├── compiler/         analyzer, rules, ir, codegen, merger
│   ├── platform/         windows, linux (macos, android: post-V1)
│   └── runtime/          C++ support runtime for generated code
├── designer/             web (Wasm viewer) + native (IPC host)
├── tests/                unit, integration, golden, fixtures, benchmarks
├── samples/              click-demo, compiler-demo, parity-demo (hello, dashboard are empty)
├── tools/                shaders, font, build tooling
├── third_party/          vendored dependencies
├── cmake/                CMake helpers
├── scripts/              dev / CI scripts
└── .github/              CI workflows, issue templates
```

## Build

Requirements: CMake >= 3.24, a C++23 compiler, and the Vulkan SDK
(`VULKAN_SDK` set; `glslc`, the GLSL-to-SPIR-V shader compiler, in the SDK `Bin/` or on `PATH`).

Windows (MSVC 2022/2026):

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
.\build\vs2026\tools\poc\vulkan_offscreen\Release\vulkan_offscreen_poc.exe --out offscreen.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend vulkan --out batched2d.png
```

Manual fallback: `cmake -S . -B build -G "Visual Studio 18 2026" -A x64` (or
`"Visual Studio 17 2022"`), then build/ctest as before.

Linux (headless, Mesa lavapipe, a software Vulkan driver):

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/vulkan_offscreen/vulkan_offscreen_poc --out offscreen.png
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/batched2d/batched2d_poc --backend vulkan --out batched2d.png
```

Tools and demos:

- `vulkan_offscreen_poc` — Vulkan 1.3 dynamic rendering into an offscreen image, read back to
  host memory, written as a PNG (no window, no swapchain).
- `batched2d_poc` — RHI + batched 2D: solid/SDF/textured quads, clip and transform stacks,
  PNG + raw RGBA output. `--backend null` runs without a GPU.
- `merger_poc` — injects developer protected regions (hand-written C++ blocks that survive regeneration) into freshly generated C++.

## Tooling

- `.clang-format`, `.clang-tidy`, `.editorconfig`
- `CMakePresets.json`: `dev`, `release`, `ci`, `vs2026`, `vs2022`
- `scripts/format.ps1` / `scripts/format.sh` (with `--check`)
- `scripts/lint.ps1` / `scripts/lint.sh`
- Compiler cache: `TESSERA_USE_COMPILER_CACHE=ON` (prefers `sccache`, supports `ccache`)
- Linux: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`
- Windows: `cmake --preset vs2026 && cmake --build --preset vs2026-release`

## Renderer detail

![Batched 2D reference](docs/images/batched2d.png)

- Thin RHI + null backend (records commands, no GPU) + Vulkan 1.3 backend using **volk** (Vulkan loader library) and **VMA** (Vulkan Memory Allocator).
- Batched 2D: solid quads, SDF (signed distance field) rounded rects, textured quads, scissor clip stack, affine
  transform stack.
- Scene graph with hit testing and W3C-style capture/target/bubble events.
- Offscreen demo writes a deterministic PNG and raw RGBA; CI runs a Vulkan smoke test (a short render with no pixel comparison).
- Big Step B2 runtime foundations: full-CSS stylesheet engine, Yoga layout, text-measurement stub,
  input normalization/dispatch, fixed-step loop, dirty rendering/damage, FPS cap/vsync/VRR, frame
  pacing, diagnostics, and the animation runtime (transitions, keyframes, springs,
  pause/fills/stop/restart, reduced motion, loop bridge).
- Defaults are **power-aware**: static UI idles (no presents/GPU work), no busy-wait, no
  full-GPU target; continuous/benchmark modes are explicit.
- Big Step B2/B3 design: async/await runtime (no UI blocking), resize/DPI/lifecycle with OS compliance,
  normalized OS events across desktop/mobile/foldables, structured diagnostics/tracing/profiling.
- Big Step B2 next: real text stack (font loading, shaping, glyph atlas), lavapipe pixel goldens,
  HAL clock/wait integration, and a damage-aware batched redraw path.

## Next

Open work is tracked as GitHub issues grouped by the eight V1 milestones, and in the checklist in
[AGENTS.md](AGENTS.md). Decisions are in [`decisions/`](decisions/README.md) (ADR-001 to ADR-053).

## License

MIT. See [LICENSE](LICENSE).

Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The canonical dependency map is
[docs/specs/dependencies.md](docs/specs/dependencies.md).
