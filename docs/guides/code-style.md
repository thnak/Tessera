# C++ code style and build-cache-friendly structure

This guide implements [ADR-015](../../decisions/ADR-015-code-style-build-cache.md) (code style and build cache). Project-wide rules are in [AGENTS.md](../../AGENTS.md).

The goal is a boringly consistent codebase that compiles incrementally and caches well.
Formatting and linting are enforced by tools, not by review comments.

## Language baseline

- **C++23** for all shipped code ([ADR-038](../../decisions/ADR-038-cpp23-baseline.md)).
- No RTTI/reflection on hot paths (hot path = code that runs per frame or per input event). No exceptions in frame-critical code.
- Prefer the standard library (`std::span`, `std::string_view`, `std::expected` where the compiler provides it)
  over ad-hoc utility types.

## Files and size

Short files are a hard maintainability and build-cache rule.

| Rule | Guideline |
|---|---|
| Hand-written `.hpp`/`.cpp` | **soft limit 300 lines**, hard limit 500 |
| Generated component | one `.g.h` + `.g.cpp` per component (component = generated UI unit, see [component-model-v1](../specs/component-model-v1.md)); split at ~500 lines |
| Primary type per header | one public type; small helpers go in `detail/` |
| Translation unit | one concern; no `all.cpp` / mega-units |
| Header weight | public headers stay light; no Vulkan/third-party headers in public APIs |

When a file crosses the soft limit, split by responsibility (for example `offscreen_renderer.cpp` →
`device.cpp`, `pipeline.cpp`, `readback.cpp`). Do not split just to hit a number; split along a
real boundary.

## Naming

| Kind | Style | Example |
|---|---|---|
| Namespace | `lower_case` | `tessera::rendering::vulkan` |
| Type / class / struct | `PascalCase` | `OffscreenRenderer` |
| Function / method | `snake_case` | `render_to_png()` |
| Variable / parameter | `snake_case` | `frame_index` |
| Member variable | `snake_case_` | `device_`, `pipeline_layout_` |
| Constant | `kPascalCase` | `kMaxFramesInFlight` |
| Enum class | `PascalCase` type, `PascalCase` values | `enum class Color { Red, Green }` |
| Macro | `TESSERA_UPPER_SNAKE` | `TESSERA_ASSERT` |
| File | `snake_case` | `offscreen_renderer.cpp` |

## Headers and includes

- `#pragma once`.
- Include what you use; do not rely on transitive includes.
- Own header first in a `.cpp`; then other project headers; then C++ standard; then
  third-party headers. `clang-format` enforces the blocks (`.clang-format`).
- Forward-declare types in headers when possible. Use PIMPL for heavy dependencies.
- Never include `<vulkan/vulkan.h>` from a public UI header; hide it behind the RHI (render hardware interface, the only GPU seam) or a backend.
- No `using namespace` in headers. In `.cpp`, scope it narrowly.
- Keep the include graph stable: a header change should not trigger a rebuild of unrelated targets.

## Errors, ownership, const

- Exceptions at API boundaries; return `std::expected`/status for recoverable hot paths.
- `std::unique_ptr` for ownership; `std::shared_ptr` only for genuinely shared lifetime.
- Non-owning views: `std::span`, `std::string_view`.
- `[[nodiscard]]` on functions whose result matters; `noexcept` where it is a real contract.
- `const` correctness everywhere; pass by value for cheap types, by `const&` for others.

## Comments and tests

- Comments explain **why**, not what. No commented-out code.
- One test file per component; deterministic; no wall clock, no network, no system fonts.
- Every protected-region change keeps the merger idempotency test green.

## Build-cache-friendly compilation

The build must be fast to rerun after a small edit.

1. **Small translation units.** Prefer many small `.cpp` files over one large one.
2. **Stable headers.** Public headers are small and change rarely. Heavy third-party headers
   live in `.cpp` or `detail/`.
3. **No PCH (precompiled header) by default.** If a PCH is introduced, it must contain only stable system/third-party
   headers, and it must be documented and measured.
4. **No unity builds by default.** Unity reduces first-build time but destroys incremental
   caching and hides missing includes.
5. **Compiler cache.** `TESSERA_USE_COMPILER_CACHE=ON` auto-detects `sccache`/`ccache` and sets
   `CMAKE_CXX_COMPILER_LAUNCHER`. CI uses the same launcher.
6. **Ninja** for local and CI builds (`CMakePresets.json`).
7. **No `-march=native` in default presets**; it makes cache entries non-portable. Keep it in a
   dedicated local perf preset only.
8. **Deterministic generated code.** No timestamps, absolute paths, random ids, or changing
   version strings in generated output.
9. **Per-component generation.** One component change regenerates one file pair, not the whole
   project.
10. **Reproducible flags.** Use `-ffile-prefix-map`/`-fdebug-prefix-map` in every build that embeds paths; avoid
    `__DATE__`, `__TIME__`, and unmapped `__FILE__` in generated code.
11. **`compile_commands.json`** is exported for clang-tidy and editors
    (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`).

## Tooling (enforced)

| Tool | File | Purpose |
|---|---|---|
| clang-format | `.clang-format` | Formatting; CI runs `--dry-run --Werror` |
| clang-tidy | `.clang-tidy` | Static analysis; naming and bugprone checks |
| EditorConfig | `.editorconfig` | Whitespace, line endings, charset |
| CMake presets | `CMakePresets.json` | Reproducible `dev`, `release`, `ci`, VS presets |
| Compiler cache | `cmake/CompilerCache.cmake` | Auto-detect `sccache`/`ccache` |
| Warning policy | `CMakeLists.txt` (`TESSERA_WARNINGS_AS_ERRORS`) | `-Werror`/`/WX` on by default; CI keeps it on |
| Format script | `scripts/format.ps1`, `scripts/format.sh` | Format or check the tree |
| Lint script | `scripts/lint.ps1`, `scripts/lint.sh` | Run clang-tidy over the tree |

Local commands:

```bash
# format everything
./scripts/format.sh
# check only (CI mode)
./scripts/format.sh --check
# lint (requires a build with compile_commands.json)
./scripts/lint.sh
```

```powershell
.\scripts\format.ps1
.\scripts\format.ps1 -Check
.\scripts\lint.ps1
```

## Generated code style

- Header comment: `// Generated by Tessera. Do not edit outside protected regions.`
- `clang-format` is applied with the repository style before merging.
- Stable ordering: components sorted by path; nodes in source order; bindings sorted by id.
- One component per file pair; no "generated everything" file.
- Protected regions (developer-written C++ blocks that survive regeneration) follow [ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md) exactly.
- No `SPDX-License-Identifier` forced into generated user code; the project's own license and
  `THIRD_PARTY_NOTICES` cover it. (Repository source files themselves carry
  `SPDX-License-Identifier: MIT`.)

## Definition of done

This list is the style-related subset of the full definition of done in [AGENTS.md](../../AGENTS.md#definition-of-done-every-change).

- `scripts/format.* --check` passes.
- Build with `TESSERA_WARNINGS_AS_ERRORS=ON`; fix warnings instead of adding blanket suppressions.
- `clang-tidy` reports no new warnings in changed files.
- Files stay under the soft limit unless there is a documented reason (the hard limit of 500 lines has no exceptions except the Phase 0 PoC targets listed in AGENTS.md).
- No new heavy include in a public header.
- A clean build and an incremental rebuild both work with the compiler cache.
