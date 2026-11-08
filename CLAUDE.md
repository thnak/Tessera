# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`AGENTS.md` is the authoritative operating manual (non-negotiables, Big Step roadmap, definition of done, full docs table). Read it first; this file only adds the commands and the big picture.

## Status

Pre-alpha. Big Step B1 (RHI = render hardware interface, Vulkan batched 2D, scene graph) is done; **B2 is the current focus** (Big Steps A-F are defined in [AGENTS.md, Big Steps](AGENTS.md#big-steps-group-work-dont-micro-step)) (stylesheet engine → Yoga → input → fixed-step loop → dirty rendering → pacing → power → async → diagnostics → text → lavapipe goldens, where lavapipe is Mesa's software Vulkan driver and a golden is a deterministic reference image). Much of the repo (`docs/specs/`, `docs/design/`, `decisions/`, the ADR = Architecture Decision Record folder, index at [decisions/README.md](decisions/README.md)) is design documentation for subsystems not yet implemented; several `src/` dirs (e.g. `src/runtime`) are empty (checked: `src/runtime` has no files). Check the code before assuming a documented feature exists. Keep the B1 RHI, batched 2D renderer, and scene graph stable.

## Commands

Presets live in `CMakePresets.json` (`dev` = Ninja Debug, `release`, `ci`, `vs2026`, `vs2022`). Windows needs the Vulkan SDK (`VULKAN_SDK` set).

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
ctest --preset vs2026-release -R scene_graph      # single test (names: merger_unit, rhi_null, scene_graph, batched2d, ...)
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend vulkan --out batched2d.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend null
.\scripts\format.ps1 -Check
.\scripts\lint.ps1 -BuildDir build\dev
```

Linux: same with `--preset dev`; Vulkan runs headless via Mesa lavapipe (a software Vulkan ICD, i.e. installable client driver, selected with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`). Use `bash scripts/format.sh --check` and `BUILD_DIR=build/dev bash scripts/lint.sh` (clang-tidy needs the Ninja `compile_commands.json`).

Unit tests are registered in `tests/unit/CMakeLists.txt` via `add_test` as one executable per area (`*_tests.cpp`).

## Architecture (big picture)

Pipeline: JSX/TSX → (Tier-1 React rules, Tier-2 `bind::` extractor; see [docs/design/compiler.md](docs/design/compiler.md)) → **IR** (intermediate representation, [docs/specs/ir-v1.md](docs/specs/ir-v1.md)) → C++ codegen → native app. The same C++ engine also compiles to Wasm (WebAssembly) + WebGPU for design-time preview. Production has no JS/React runtime.

- **Compiler** (`src/compiler/{analyzer,rules,ir,codegen,merger}`): codegen consumes IR only, never the parser AST. The **merger** preserves developer C++ inside `[DEVELOPER_LOGIC_START: bind::id]` protected regions and must be idempotent (`merge(merge(g,e),e) == merge(g,e)`); any change needs the idempotency test updated.
- **Rendering**: `include/tessera/rendering/rhi/` + `src/rendering/rhi/` is the *only* seam to a GPU API. Backends: `src/rendering/backend/{vulkan,webgpu}` and `src/rendering/null` (deterministic command tests). The batched 2D renderer (`include/tessera/rendering/gpu/`, `src/rendering/gpu/`) sits on the RHI. Vulkan is native; WebGPU is Wasm-only — never Vulkan in Wasm.
- **UI** (`src/ui/{view,layout,style,scene,text,animation,...}`): scene graph with hit-testing and W3C capture/target/bubble events; Yoga for layout, a CSS-compatible stylesheet engine, dirty-flag domains (style/layout/paint/render; see [docs/design/dirty-rendering.md](docs/design/dirty-rendering.md)).
- **HAL/platform** (`src/hal`, `src/platform/*`): OS specifics are normalized here; UI code sees `WindowMetrics`, never window handles or swapchains ([ADR-020](decisions/ADR-020-window-resize-and-lifecycle.md), window resize and lifecycle).
- **Goldens**: pixel goldens come from lavapipe/CPU rasterizer only, never a real GPU.

## Constraints that bite when coding

- Static linking of third-party libs (no LGPL/GPL); MIT project license. New dependency → entries in `docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md`.
- Never block the UI thread (no `vkWaitForFences`/`vkDeviceWaitIdle`, blocking I/O); no detached tasks; static UI must idle with zero presents.
- No hidden allocations or RTTI on frame/hot paths; exceptions at boundaries, `std::expected` for recoverable hot paths; never throw across threads/FFI.
- New files: `#pragma once`, namespace `tessera::<area>::<module>`, `SPDX-License-Identifier: MIT`, soft 300 / hard 500 line limit, one primary type per header. C++23, `.clang-format` (4 spaces, 100 cols).
- Shaders are GLSL → SPIR-V via `glslc`.
- Every new subsystem gets a design note in `docs/design/` (ADR in `decisions/` if contentious) and a `docs/README.md` update.
