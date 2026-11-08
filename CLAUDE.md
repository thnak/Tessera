# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`AGENTS.md` is the authoritative operating manual (non-negotiables, Big Step roadmap, definition of done, full docs table). Read it first; this file only adds the commands and the big picture.

## Status

Pre-alpha. Big Step B1 (RHI = render hardware interface, Vulkan batched 2D, scene graph) is done; **B2 is the current focus** (Big Steps A-F are defined in [AGENTS.md, Big Steps](AGENTS.md#big-steps-group-work-dont-micro-step)) (stylesheet engine → Yoga → input → fixed-step loop → dirty rendering → pacing → power → async → diagnostics → text → lavapipe goldens, where lavapipe is Mesa's software Vulkan driver and a golden is a deterministic reference image). The B2 foundation is started: fixed-step accumulator (`src/runtime/fixed_step.*`), clock interface + deterministic `ManualClock` (`src/runtime/clock.*`), `RuntimeLoop` render-on-demand/wake state (`src/runtime/runtime_loop.*`), `FramePacer` deadline scheduling (`src/runtime/frame_pacer.*`), normalized input queue/coalescing (`src/ui/input/`), scene dirty flags (`include/tessera/ui/dirty.hpp`, `src/ui/scene/node.cpp`), pruned dirty collect/clear (`src/ui/scene/dirty_pass.cpp`), damage accumulation (`src/rendering/damage/damage.cpp`), and CSS tokenization (`src/ui/style/tokenizer.cpp`). Much of the repo (`docs/specs/`, `docs/design/`, `decisions/`, index at [decisions/README.md](decisions/README.md)) is design documentation for subsystems not yet implemented; check the code before assuming a documented feature exists. Keep the B1 RHI, batched 2D renderer, and scene graph stable.

## Execution loop

Work continuously, not stop-and-wait. A parallel clone can ship the same idea; keep a green `main`
and land verified increments.

- **One Big Goal per cycle**, normally the next unblocked B2 slice.
- **Split into testable slices**; each slice = behavior + tests + docs.
- **Commit every green slice** and use multiple commits per part (model → algorithm → integration →
  tests/docs). Never leave green work uncommitted.
- **After a commit, take the next slice automatically**; only stop for destructive/ambiguous
  decisions, blocked external CI, or context exhaustion.
- On context exhaustion: commit the current green slice and write the exact next slice in
  Current focus.

A commit is a checkpoint, not a finish line.

## Commands

Presets live in `CMakePresets.json` (`dev` = Ninja Debug, `release`, `ci`, `vs2026`, `vs2022`). Windows needs the Vulkan SDK (`VULKAN_SDK` set).

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
ctest --preset vs2026-release -R scene_graph      # single test (names: merger_unit, rhi_null, scene_graph, scene_dirty, fixed_step, batched2d, ...)
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend vulkan --out batched2d.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend null
.\scripts\format.ps1 -Check
.\scripts\lint.ps1 -BuildDir build\dev
```

Linux: same with `--preset dev`; Vulkan runs headless via Mesa lavapipe (a software Vulkan ICD, i.e. installable client driver, selected with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`). Use `bash scripts/format.sh --check` and `BUILD_DIR=build/dev bash scripts/lint.sh` (clang-tidy needs the Ninja `compile_commands.json`).

Unit tests are registered in `tests/unit/CMakeLists.txt` via `add_test` as one executable per area (`*_tests.cpp`). GPU tests live under `tests/gpu/` and are registered in `tests/gpu/CMakeLists.txt`.

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
- Shaders are GLSL → SPIR-V via `glslc`; no runtime shader compilation in production.

## Code style

Source of truth: [docs/guides/code-style.md](docs/guides/code-style.md) (ADR-015, ADR-038), summarized in [AGENTS.md](AGENTS.md#code-conventions).

- **C++23** everywhere; `std::expected<T, Error>` for recoverable hot paths.
- **Files:** soft 300 / hard 500 lines, one primary type per header; split by responsibility. Phase 0 PoC targets are the only hard-limit exceptions.
- **Naming:** `PascalCase` types, `snake_case` functions/variables, `snake_case_` members, `kPascalCase` constants, `TESSERA_UPPER_SNAKE` macros.
- **Headers:** `#pragma once`, include what you use, no `using namespace` in headers, no GPU/third-party headers in public UI headers.
- **Errors/ownership:** `unique_ptr` for ownership, `shared_ptr` only for genuinely shared lifetime; no RTTI/reflection or hidden allocations on hot paths.
- **Comments:** explain why, not what; no commented-out code.
- **Format/lint:** run `scripts/format.* --check` and `scripts/lint.*`; CI enforces `.clang-format` and `.clang-tidy`.
- **Warnings:** `TESSERA_WARNINGS_AS_ERRORS=ON` by default (`-Werror`/`/WX`); fix warnings instead of suppressing them, except narrow third-party `-w`/`/W0` sources.
- **Tests:** deterministic, one component per file, no wall clock/network/system fonts/random ids; GPU tests live in `tests/gpu/` and goldens in `tests/golden/gpu/`.

## Documentation rules

Source of truth: [AGENTS.md](AGENTS.md#documentation-rules) and [docs/README.md](docs/README.md).

- Stage docs by type: `docs/architecture/`, `docs/specs/` (normative), `docs/design/` (rationale), `docs/guides/` (how-to), `docs/reference/`, `decisions/` (ADR).
- **Size:** target 400 lines, hard limit **600 lines**. Over 600 → a folder with a `README.md` index and linked parts (see `docs/design/gpu/`).
- Update [docs/README.md](docs/README.md) for every add/move/rename; update [decisions/README.md](decisions/README.md) for every ADR.
- Use relative links; keep headings stable; no orphan docs; code fences must balance.
- One normative source per rule; design docs explain and link instead of copying.
- New subsystem → design note; contentious/hard-to-reverse decision → ADR; new artifact/format → versioning spec.
- Docs-only changes still run the link/index/fence/size checks.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for setup, build/test commands, branch/commit style, PR checklist, and the docs/code rules above. The canonical remote is <https://github.com/thnak/Tessera.git>.
