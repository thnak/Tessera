# AGENTS.md — Tessera

Working name. This file is the operating manual for any coding agent (and for humans)
working in this repo. Read it before touching code.

## What this project is

A cross-platform C++ UI framework:

- **DSL**: standard React/JSX syntax (`useState`, `useEffect`, `useRef`, `useAnimatedValue`).
- **Production**: a two-tier compiler turns JSX → IR → **native C++** source.
- **Design time**: the same C++ engine compiled to **WebAssembly + WebGPU** for instant,
  emulator-free preview in a browser.
- **Native backend**: **Vulkan 1.3 is the primary renderer**. WebGPU/Dawn is the browser backend.
- **Core model**: game-engine-style fixed timestep, zero-allocation buffers, retained/immediate
  hybrid, dirty-flag invalidation.
- **AI-friendly**: deterministic offscreen framebuffer + golden-image diff, usable by agents.

## Non-negotiables

1. **Vulkan 1.3 native, WebGPU for WebAssembly.** Never attempt to run Vulkan inside Wasm.
   Two backends, one thin RHI, one shared golden-test corpus.
2. **Buy before build.** Do not re-implement solved infrastructure:
   - Layout: **Yoga** (Flexbox). Grid: small subset or taffy.
   - Vector paths: **ThorVG** or **Skia Graphite** (Vulkan). Never write a path rasterizer in V1.
   - Text: **FreeType + HarfBuzz + SheenBidi + ICU**.
   - GPU memory: **VMA**. Loader: **volk** / `vk-bootstrap`.
   - Browser GPU: **Dawn** (WebGPU).
   The canonical list, with license and module ownership, is
   [`docs/specs/dependencies.md`](docs/specs/dependencies.md). Add every new dependency there.
3. **Thin RHI.** `src/rendering/rhi/` is the only seam between UI/rendering and a GPU API.
   Backends live under `src/rendering/backend/{vulkan,webgpu,null}`.
4. **Compiler is IR-first.** JSX/TSX → `src/compiler/ir` → C++ codegen. Never make codegen
   read the raw parser AST directly.
5. **Protected regions are idempotent.**
   `merge(merge(generated, existing), existing) == merge(generated, existing)`.
   Developer C++ inside `[DEVELOPER_LOGIC_START: bind::id]` must survive UI regeneration.
6. **Deterministic headless tests use a software path.** Golden images come from
   Mesa **lavapipe** or the CPU rasterizer, never from a real GPU framebuffer.
7. **No React runtime in production.** Production is pure C++. React semantics exist only
   at design time (custom reconciler in the Wasm viewer).
8. **V1 scope discipline.** Support a small, explicit hooks subset and reject unsupported
   patterns loudly. Do not grow the surface before Big Step C is proven.
9. **Static linking is a hard requirement** (ADR-012). Tessera and third-party C/C++ libraries
   are linked statically into the app. System components (Vulkan loader/ICDs, OS frameworks)
   stay dynamic. No LGPL/GPL in statically linked code.
10. **Project license is MIT** (ADR-013). Keep shipped dependencies permissive; preserve
    third-party notices. Every new dependency needs an entry in
    [`docs/specs/dependencies.md`](docs/specs/dependencies.md) and
    [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
11. **Power-aware by default** (ADR-019). This is an application framework, not a benchmark.
    Static UI must idle (no presents/GPU work), the loop must never busy-wait, and adaptive/
    on-demand defaults are required. Full-GPU utilization is not a goal.
12. **Never block the UI thread** (ADR-021). Use async/await plus bounded state machines; no
    `vkWaitForFences`/`vkDeviceWaitIdle`, blocking I/O, or indefinite acquire/present on the UI
    thread. No detached tasks.
13. **OS compliance lives in the HAL/runtime** (ADR-020). Styles/animations/components see
    `WindowMetrics`, never window handles, OS messages, or swapchains. Resize/DPI/lifecycle are
    normalized and tested per platform.
14. **Diagnostics are structured, bounded, and optional** (ADR-022). No printf-only debugging;
    logs/traces/metrics/crash/capture are privacy-safe and cheap when disabled.
15. **Native-first boundaries; no production bridges** (ADR-024). The UI/render/input/memory
    hot path is direct C++. No JS/React/scripting runtime, virtual DOM, per-frame FFI/IPC, or
    UI objects across JNI/ObjC/Wasm. Bridges are only at OS/build-time/design-time edges; see
    [`docs/design/bridges.md`](docs/design/bridges.md) and
    [`docs/specs/memory-model-v1.md`](docs/specs/memory-model-v1.md).
16. **Error boundaries are the catch point** (ADR-025). Catch synchronous exceptions from
    generated component code at a boundary; async errors are values routed to the owning
    boundary; never throw across threads/FFI. No silent catch; fatal/OOM/cancellation/platform
    errors go to the root handler. See
    [`docs/specs/error-boundaries-v1.md`](docs/specs/error-boundaries-v1.md).

## Big Steps (group work, don't micro-step)

Each Big Step ends with a demoable, committed result. Do not start the next one before the
current acceptance criteria pass.

### Big Step A — Foundation & risk proof *(complete)*
Repo + build + the two riskiest proofs.
- [x] CMake project builds on Windows (MSVC 2022/2026) and Linux.
- [x] Vulkan 1.3 offscreen renderer → PNG (dynamic rendering, no window/swapchain).
- [x] Same PNG runs headless in CI with Mesa lavapipe.
- [x] Protected-region merger prototype, idempotent, with unit tests.
- [x] `AGENTS.md`, README, CI workflow.

### Big Step B — Core renderer *(B1 complete; B2 next)*
- [x] Thin RHI: device/buffer/texture/pipeline/render target/command list.
- [x] Null backend for deterministic command tests.
- [x] Vulkan backend: instance/device/queue, volk + VMA, dynamic rendering, descriptor sets,
      pipeline, offscreen target, readback.
- [x] Batched 2D: solid quads, SDF rounded rects, textured quads, clip stack, transform stack.
- [x] Scene graph, hit-testing, W3C-style capture/target/bubble events.
- [x] Acceptance: static dashboard rendered offscreen → [`docs/images/batched2d.png`](docs/images/batched2d.png);
      CI smoke test green.
- [ ] **B2:** Stylesheet engine: full CSS syntax + compatibility matrix, cascade, custom
      properties, pseudo-classes (ADR-016).
- [ ] **B2:** Yoga Flexbox adapter (`src/ui/layout/`).
- [ ] **B2:** FreeType/HarfBuzz/SheenBidi/ICU + glyph atlas + text batching.
- [ ] **B2:** Input handler: normalized events, focus, pointer capture, pseudo-state dispatch
      (ADR-017).
- [ ] **B2:** Fixed-step runtime loop: accumulator, time scale, deterministic mode (ADR-017).
- [ ] **B2:** Dirty rendering: style/layout/paint/render domains, caches, damage (ADR-018).
- [ ] **B2:** Frame pacing: `maxFps`, vsync modes, dynamic refresh/VRR, render-on-demand
      (ADR-018).
- [ ] **B2:** Power-aware defaults: idle/on-demand, adaptive quality, occlusion/background
      policy, no busy-wait/full-load (ADR-019).
- [ ] **B2:** Async/await runtime: executors, cancellation, bounded completion queues, no UI
      blocking (ADR-021).
- [ ] **B2:** Diagnostics: structured logging, tracing, metrics, crash dumps, overlay (ADR-022).
- [ ] **B2:** Pixel goldens via lavapipe/software path (ADR-009).
- [ ] **B2:** Multi-frame synchronization (frames in flight, timeline semaphores).
- [ ] **B3:** Window/swapchain path: resize/DPI/lifecycle, OS compliance, old-swapchain
      retirement (ADR-020).
- [ ] **B3:** Normalized OS events across desktop/mobile/foldables, display capabilities,
      fold posture/hinge/occlusion (ADR-023).

### Big Step C — Compiler vertical slice
- [ ] JSX/TSX parser adapter (SWC or oxc) + Tier-1 React rules.
- [ ] Tier-2 binding extractor (`bind::`) + IR.
- [ ] C++ codegen + incremental merger.
- [ ] Acceptance: one sample screen with `useState`/`useEffect`/`useRef` regenerates without
      losing hand-written C++ logic.

### Big Step D — Design-time preview
- [ ] C++ engine → Wasm (Emscripten) + WebGPU/Dawn surface.
- [ ] Custom React reconciler + IPC bridge + hot reload.
- [ ] Acceptance: same screen previews in a browser and matches native output.

### Big Step E — Data-heavy UI
- [ ] Fixed timestep game clock + time scale under sustained load.
- [ ] Zero-allocation ring buffers for chart series.
- [ ] Retained/immediate hybrid + per-node dirty flags + damage rects.
- [ ] FPS cap, vsync/VRR, render-on-demand, and frame-budget metrics.
- [ ] Acceptance: streaming dashboard at target frame rate with deterministic goldens.

### Big Step F — Production breadth
- [ ] Windows + Linux complete; macOS via MoltenVK.
- [ ] Accessibility (UIA / AT-SPI / NSAccessibility).
- [ ] Mobile integration (Android first; iOS later): surface, input, IME, accessibility,
      lifecycle, manifest/Info.plist declarations. See
      [`docs/design/mobile-integration.md`](docs/design/mobile-integration.md).
- [ ] Services (i18n, navigation, storage, networking), diagnostics, packaging.
- [ ] OS lifecycle compliance (resize/DPI/occlusion), crash reporting, profiling on Windows/Linux.
- [ ] Acceptance: a real app ships, passes validation layers and golden tests in CI.

## Build & test

Windows (Vulkan SDK required; `VULKAN_SDK` is set):
```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
.\build\vs2026\tools\poc\vulkan_offscreen\Release\vulkan_offscreen_poc.exe --out offscreen.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend vulkan --out batched2d.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend null
```

Linux:
```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/vulkan_offscreen/vulkan_offscreen_poc --out offscreen.png
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/batched2d/batched2d_poc --backend vulkan --out batched2d.png
./build/dev/tools/poc/batched2d/batched2d_poc --backend null
```

Formatting and linting:
```powershell
.\scripts\format.ps1 -Check
.\scripts\lint.ps1 -BuildDir build\dev
```
```bash
bash scripts/format.sh --check
BUILD_DIR=build/dev bash scripts/lint.sh
```

The manual `cmake -S . -B build ...` path still works; presets are the recommended route.

## Where things go

| Area | Path |
|---|---|
| Architecture decisions | `decisions/` — start at [decisions/README.md](decisions/README.md) |
| Documentation map | [`docs/README.md`](docs/README.md) |
| Architecture overview | [`docs/architecture/overview.md`](docs/architecture/overview.md) |
| Module map | [`docs/architecture/module-map.md`](docs/architecture/module-map.md) |
| Getting started | [`docs/guides/getting-started.md`](docs/guides/getting-started.md) |
| Developer guide | [`docs/guides/project-generator.md`](docs/guides/project-generator.md) |
| Developer workflow / protected regions | [`docs/guides/developer-workflow.md`](docs/guides/developer-workflow.md) |
| Project config schema | [`docs/specs/project-config-v1.md`](docs/specs/project-config-v1.md) |
| Code style & build cache | [`docs/guides/code-style.md`](docs/guides/code-style.md) |
| Dependency map | [`docs/specs/dependencies.md`](docs/specs/dependencies.md) |
| Public headers | `include/tessera/` |
| Core primitives | `src/core/` |
| Platform seam | `src/hal/` + `src/platform/{windows,linux,macos,android}` |
| UI model | `src/ui/` (view, layout, style, animation, text, accessibility) |
| RHI (only GPU seam) | `include/tessera/rendering/rhi/` + `src/rendering/rhi/` |
| Batched 2D renderer | `include/tessera/rendering/gpu/` + `src/rendering/gpu/` |
| Null backend | `src/rendering/null/` |
| Vulkan RHI backend | `src/rendering/backend/vulkan/` |
| WebGPU backend | `src/rendering/backend/webgpu/` |
| Software/headless | `src/rendering/software/` |
| Scene graph / events | `include/tessera/ui/scene/` + `src/ui/scene/` |
| Stylesheet spec | [`docs/specs/stylesheet-v1.md`](docs/specs/stylesheet-v1.md) |
| Runtime config | [`docs/specs/runtime-config-v1.md`](docs/specs/runtime-config-v1.md) |
| Runtime loop / pacing | [`docs/design/game-loop.md`](docs/design/game-loop.md) |
| Dirty rendering | [`docs/design/dirty-rendering.md`](docs/design/dirty-rendering.md) |
| Performance / power | [`docs/design/performance.md`](docs/design/performance.md) |
| Window resize/lifecycle | [`docs/design/window-resize.md`](docs/design/window-resize.md) |
| OS events / foldables | [`docs/specs/os-events-v1.md`](docs/specs/os-events-v1.md) |
| Async runtime | [`docs/design/async.md`](docs/design/async.md) |
| Diagnostics/profiling | [`docs/design/diagnostics.md`](docs/design/diagnostics.md) |
| Diagnostics config | [`docs/specs/diagnostics-config-v1.md`](docs/specs/diagnostics-config-v1.md) |
| Input handler | [`docs/design/input.md`](docs/design/input.md) |
| Renderer design notes | `docs/design/renderer.md`, `docs/design/scene-graph.md`, `docs/design/antialiasing.md` |
| UI model / layout / text / animation | `docs/design/ui-model.md`, `layout.md`, `text.md`, `animation.md` |
| Responsive / accessibility | `docs/design/responsive.md`, `docs/design/accessibility.md` |
| HAL | [`docs/design/hal.md`](docs/design/hal.md) |
| Mobile integration | [`docs/design/mobile-integration.md`](docs/design/mobile-integration.md) |
| Compiler / IR / component model | [`docs/design/compiler.md`](docs/design/compiler.md), [`docs/specs/ir-v1.md`](docs/specs/ir-v1.md), [`docs/specs/component-model-v1.md`](docs/specs/component-model-v1.md) |
| UI virtualization | [`docs/specs/virtualization-v1.md`](docs/specs/virtualization-v1.md) |
| Designer / Wasm | [`docs/design/designer.md`](docs/design/designer.md) |
| Threading / memory | [`docs/design/threading-and-memory.md`](docs/design/threading-and-memory.md) |
| Memory model / bridges | [`docs/specs/memory-model-v1.md`](docs/specs/memory-model-v1.md), [`docs/design/bridges.md`](docs/design/bridges.md) |
| Buffer config / tunables | [`docs/specs/buffer-config-v1.md`](docs/specs/buffer-config-v1.md), [`docs/guides/tunables.md`](docs/guides/tunables.md) |
| Startup / loading / auth | [`docs/design/startup-and-loading.md`](docs/design/startup-and-loading.md), [`docs/guides/loading-matrix.md`](docs/guides/loading-matrix.md) |
| Resource lifecycle | [`docs/specs/resource-lifecycle-v1.md`](docs/specs/resource-lifecycle-v1.md) |
| Error boundaries | [`docs/specs/error-boundaries-v1.md`](docs/specs/error-boundaries-v1.md) |
| Deep links / activation | [`docs/specs/deep-linking-v1.md`](docs/specs/deep-linking-v1.md), [`docs/guides/deep-linking.md`](docs/guides/deep-linking.md) |
| Automation IDs / headless / audits | [`docs/specs/automation-ids-v1.md`](docs/specs/automation-ids-v1.md), [`docs/specs/headless-automation-v1.md`](docs/specs/headless-automation-v1.md), [`docs/specs/lighthouse-audit-v1.md`](docs/specs/lighthouse-audit-v1.md) |
| Custom native elements / 3D | [`docs/specs/custom-elements-v1.md`](docs/specs/custom-elements-v1.md), [`docs/guides/custom-elements.md`](docs/guides/custom-elements.md) |
| Text editing / forms | [`docs/specs/text-editing-v1.md`](docs/specs/text-editing-v1.md), [`docs/specs/forms-and-validation-v1.md`](docs/specs/forms-and-validation-v1.md) |
| Scroll / gestures / overlays | [`docs/specs/scroll-and-gestures-v1.md`](docs/specs/scroll-and-gestures-v1.md), [`docs/specs/overlays-and-layers-v1.md`](docs/specs/overlays-and-layers-v1.md) |
| Navigation / restore / shell / i18n | [`docs/specs/navigation-v1.md`](docs/specs/navigation-v1.md), [`docs/specs/state-restoration-v1.md`](docs/specs/state-restoration-v1.md), [`docs/specs/platform-shell-v1.md`](docs/specs/platform-shell-v1.md), [`docs/specs/i18n-v1.md`](docs/specs/i18n-v1.md) |
| Accessibility / async data | [`docs/specs/accessibility-v1.md`](docs/specs/accessibility-v1.md), [`docs/specs/async-data-v1.md`](docs/specs/async-data-v1.md) |
| Devtools / updates / crash | [`docs/specs/devtools-protocol-v1.md`](docs/specs/devtools-protocol-v1.md), [`docs/specs/updates-and-distribution-v1.md`](docs/specs/updates-and-distribution-v1.md), [`docs/specs/crash-reporting-v1.md`](docs/specs/crash-reporting-v1.md) |
| Cross-check / gaps | [`docs/architecture/app-completeness-cross-check-v1.md`](docs/architecture/app-completeness-cross-check-v1.md), [`docs/architecture/app-completeness-gap-resolutions-v1.md`](docs/architecture/app-completeness-gap-resolutions-v1.md) |
| Resources / services / security | `docs/design/resources.md`, `services.md`, `security.md` |
| Testing / goldens | [`docs/guides/testing.md`](docs/guides/testing.md), [`docs/specs/golden-tests-v1.md`](docs/specs/golden-tests-v1.md) |
| Build / packaging | [`docs/design/build-and-packaging.md`](docs/design/build-and-packaging.md), [`docs/guides/packaging.md`](docs/guides/packaging.md) |
| Glossary | [`docs/reference/glossary.md`](docs/reference/glossary.md) |
| Batched2D demo | `tools/poc/batched2d/` |
| Compiler frontend/IR/codegen | `src/compiler/{analyzer,rules,ir,codegen,merger}/` |
| Design-time viewer | `designer/{web,native}/` |
| Tests | `tests/{unit,integration,golden,fixtures,benchmarks}/` |
| PoCs / demos | `tools/poc/`, `samples/` |

## Code conventions

- **C++20** (move hot paths to C++23 only when the toolchain allows it consistently).
- Headers: `#pragma once`. Namespace: `tessera::<area>::<module>`.
- Errors: exceptions at boundaries, `std::expected` for recoverable hot paths.
- No RTTI/reflection on hot paths. No hidden allocations in frame code.
- Formatting: `.clang-format` (LLVM base, 4 spaces, 100 cols, pointer left). Run
  `scripts/format.*`; CI runs `--check`.
- Linting: `.clang-tidy`; run `scripts/lint.*` against a Ninja `compile_commands.json`.
- **Short files:** soft limit 300 lines, hard limit 500; one primary type per header
  (ADR-015, [`docs/guides/code-style.md`](docs/guides/code-style.md)).
- **Build cache:** small translation units, stable public headers, no PCH/unity by default;
  `TESSERA_USE_COMPILER_CACHE=ON` auto-detects sccache/ccache.
- Project config: `tessera.json` schema v1 (ADR-014); see
  [`docs/specs/project-config-v1.md`](docs/specs/project-config-v1.md) and
  [`docs/guides/project-generator.md`](docs/guides/project-generator.md).
- Shaders: GLSL → SPIR-V via `glslc`; SPIR-V is the source of truth for Vulkan.
- Every subsystem gets a short design note in `docs/design/` and, if contentious, an ADR
  in `decisions/`. Architecture docs live in `docs/architecture/`, normative specs in
  `docs/specs/`, and how-to guides in `docs/guides/`; update `docs/README.md` when adding docs.

## Definition of done (every change)

- [ ] Builds on Windows and Linux.
- [ ] Unit/integration tests pass (`ctest`).
- [ ] Vulkan paths run clean under validation layers (debug builds).
- [ ] No new dependency without entries in `docs/specs/dependencies.md` and
      `THIRD_PARTY_NOTICES.md`.
- [ ] Static link check passes: no unexpected third-party dynamic libraries (ADR-012).
- [ ] New source files carry `SPDX-License-Identifier: MIT`.
- [ ] `scripts/format.* --check` passes; clang-tidy reports no new warnings in changed files.
- [ ] Files stay under the 300/500-line soft/hard limits (ADR-015). Phase 0 PoC targets
      (`tools/poc/vulkan_offscreen`, `offscreen_renderer.cpp`) are exempt until retired.
- [ ] Generator/config changes update `docs/guides/project-generator.md` and the schema.
- [ ] If it touches protected regions: idempotency test updated.
- [ ] If it touches rendering: a golden test or a manual visual check is recorded.
- [ ] If it touches dirty state: dirty-only and full-redraw output are equivalent.
- [ ] If it touches pacing: frame-cap/vsync/VRR fallback behavior is recorded.
- [ ] If it touches power/idle: a static scene produces zero presents/submissions after settling.
- [ ] If it touches window/resize: synthetic resize/DPI/lifecycle tests pass per platform.
- [ ] If it touches OS events/foldables: synthetic fold/posture/display scripts pass.
- [ ] If it touches async: no blocking UI wait; cancellation/shutdown/queue-overflow tests pass.
- [ ] If it touches diagnostics: disabled paths add no measurable cost; redaction tests pass.
- [ ] If it touches buffers/tunables: schema/min/max/step/budget validation passes; user
      settings are bounded; golden output is unaffected by buffer sizes.
- [ ] If it touches startup/loading/auth: budgets and failure/offline paths are tested; no UI
      thread blocking; loading/auth screens are accessible.
- [ ] If it touches resources: lazy/preload/lease/unload/eviction tests pass; no unload of
      active/pinned resources; no leak/thrash under route churn.
- [ ] If it touches error boundaries/exceptions: fallback/retry/nested/async/event/fallback-
      failure/loop/redaction/cleanup tests pass; no throw across threads/FFI; no silent catch.
- [ ] If it touches deep links/activation: cold/warm/single-instance/auth/malformed/oversized/
      file-association/browser-fallback tests pass; no secrets in URLs/logs; no shell execution.
- [ ] If it touches automation/headless/audits: numeric ID manifest is deterministic; snapshot/
      action/trace/audit schemas pass; golden/GPU separation; audit disabled adds no cost.
- [ ] If it touches custom elements/3D: manifest/prop/event/shader validation, compile-time
      registry, leaf/reuse rule, element input/capture/custom events, offscreen/inline and
      unit/structural/golden/GPU-smoke tests pass; no raw backend/OS handles or runtime shaders.
- [ ] If it touches text editing/forms: grapheme/bidi/IME/selection/undo/clipboard/secure fields,
      sync/async validation, focus-first-invalid, accessibility and headless tests pass.
- [ ] If it touches scroll/gestures/overlays: nested scroll/gesture arena, snap/sticky/anchoring,
      modal focus/back/dismiss, anchoring/clamping and goldens/headless tests pass.
- [ ] If it touches navigation/restore/shell/i18n/a11y/data: route/back/deep-link/restore
      versioning, clipboard/DnD/dialog permissions, locale/RTL, semantics, cache/offline tests pass.
- [ ] If it touches devtools/updates/crash: local-only devtools, signed/staged/rollback updates,
      symbolicated/redacted crash reports, and release-disabled devtools tests pass.

## Current focus

**Big Step B2.** Implement the runtime slice in this order: stylesheet engine, Yoga adapter,
input handler, fixed-step loop, dirty rendering, frame pacing/FPS/VRR, power-aware idle/adaptive
defaults, async/await runtime, diagnostics/tracing, then text/glyph atlas and the
lavapipe/software pixel golden pipeline. B3 adds the window/swapchain path with resize/DPI/OS
lifecycle, normalized OS events, and mobile/foldable metrics. Keep the B1 RHI, batched 2D
renderer, and scene graph stable while doing so.
