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

## Big Steps (group work, don't micro-step)

Each Big Step ends with a demoable, committed result. Do not start the next one before the
current acceptance criteria pass.

### Big Step A — Foundation & risk proof *(current)*
Repo + build + the two riskiest proofs.
- [ ] CMake project builds on Windows (MSVC 2022) and Linux.
- [ ] Vulkan 1.3 offscreen renderer → PNG (dynamic rendering, no window/swapchain).
- [ ] Same PNG runs headless in CI with Mesa lavapipe.
- [ ] Protected-region merger prototype, idempotent, with unit tests.
- [ ] `AGENTS.md`, README, CI workflow.

### Big Step B — Core renderer
- [ ] RHI interface + Vulkan backend (instance/device/queue/image/pipeline/sync).
- [ ] Batched 2D renderer: quads, SDF rounded rects, clip stack, transforms, glyph atlas.
- [ ] Yoga Flexbox + text (FreeType/HarfBuzz).
- [ ] Scene graph, hit-testing, W3C-style capture/target/bubble events.
- [ ] Acceptance: render a static screen matching a reference PNG.

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
- [ ] Fixed timestep game clock + time scale.
- [ ] Zero-allocation ring buffers for chart series.
- [ ] Retained/immediate hybrid + per-node dirty flags.
- [ ] Acceptance: streaming dashboard at target frame rate with deterministic goldens.

### Big Step F — Production breadth
- [ ] Windows + Linux complete; macOS via MoltenVK.
- [ ] Accessibility (UIA / AT-SPI / NSAccessibility).
- [ ] Services (i18n, navigation, storage, networking), diagnostics, packaging.
- [ ] Acceptance: a real app ships, passes validation layers and golden tests in CI.

## Build & test

Windows (Vulkan SDK required; `VULKAN_SDK` is set):
```powershell
# Visual Studio 2026:
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
# Visual Studio 2022: -G "Visual Studio 17 2022"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\tools\poc\vulkan_offscreen\Release\vulkan_offscreen_poc.exe --out offscreen.png
```

Linux:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/tools/poc/vulkan_offscreen/vulkan_offscreen_poc --out offscreen.png
```

## Where things go

| Area | Path |
|---|---|
| Architecture decisions | `decisions/` — start at [decisions/README.md](decisions/README.md) |
| Public headers | `include/tessera/` |
| Core primitives | `src/core/` |
| Platform seam | `src/hal/` + `src/platform/{windows,linux,macos,android}` |
| UI model | `src/ui/` (view, layout, style, animation, text, accessibility) |
| RHI (only GPU seam) | `src/rendering/rhi/` |
| Vulkan backend | `src/rendering/backend/vulkan/` |
| WebGPU backend | `src/rendering/backend/webgpu/` |
| Software/headless | `src/rendering/software/` |
| Compiler frontend/IR/codegen | `src/compiler/{analyzer,rules,ir,codegen,merger}/` |
| Design-time viewer | `designer/{web,native}/` |
| Tests | `tests/{unit,integration,golden,fixtures,benchmarks}/` |
| PoCs / demos | `tools/poc/`, `samples/` |

## Code conventions

- **C++20** (move hot paths to C++23 only when the toolchain allows it consistently).
- Headers: `#pragma once`. Namespace: `tessera::<area>::<module>`.
- Errors: exceptions at boundaries, `std::expected` for recoverable hot paths.
- No RTTI/reflection on hot paths. No hidden allocations in frame code.
- Formatting: `.clang-format` (LLVM base, 4 spaces, 100 cols once added).
- Shaders: GLSL → SPIR-V via `glslc`; SPIR-V is the source of truth for Vulkan.
- Every subsystem gets a short design note in `docs/design/` and, if contentious, an ADR
  in `decisions/`.

## Definition of done (every change)

- [ ] Builds on Windows and Linux.
- [ ] Unit/integration tests pass (`ctest`).
- [ ] Vulkan paths run clean under validation layers (debug builds).
- [ ] No new dependency without a note in `third_party/README.md`.
- [ ] If it touches protected regions: idempotency test updated.
- [ ] If it touches rendering: a golden test or a manual visual check is recorded.

## Current focus

**Big Step A.** Do not start the scene graph, layout, or compiler frontend before the
Vulkan offscreen PNG and the merger idempotency proof are green in CI.
