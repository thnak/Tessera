# ADR-015: Code style and build-cache-friendly translation units

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-008](ADR-008-protected-regions-and-manifest.md) (protected regions and binding manifest), [ADR-012](ADR-012-static-linking.md) (static linking)

## Context

Compile time and incremental rebuild time are part of developer experience. Large translation
units, heavy public headers, precompiled headers, and unity builds can make the first build
faster while destroying incremental caching. Style also drifts without enforcement.

## Decision

1. **C++20**, formatted with `.clang-format` (LLVM base, 4 spaces, 100 columns,
   `PointerAlignment: Left`, `InsertBraces: true`).
2. Naming rules per [`docs/guides/code-style.md`](../docs/guides/code-style.md):
   `PascalCase` types, `snake_case` functions/variables, `snake_case_` private members,
   `kPascalCase` constants.
3. **Short files:** soft limit **300 lines**, hard limit **500 lines** for hand-written
   `.hpp`/`.cpp`; one primary public type per header.
4. **Public headers stay light.** No Vulkan/third-party headers in public APIs; forward
   declarations or PIMPL (pointer-to-implementation idiom) for heavy dependencies.
5. **No PCH (precompiled header) by default. No unity builds by default.** If a PCH is introduced, it may contain
   only stable system/third-party headers and must be measured and documented.
6. **Compiler cache:** `TESSERA_USE_COMPILER_CACHE=ON` auto-detects `sccache`/`ccache` and sets
   `CMAKE_CXX_COMPILER_LAUNCHER`. Prefer `sccache` (Apache-2.0) over `ccache` (GPL-3.0,
   build-time only).
7. **Ninja presets** for local and CI (continuous integration) builds; no `-march=native` in default presets.
8. **Deterministic generated code:** stable ordering, no timestamps, no absolute paths, no
   random identifiers.
9. **Enforcement:** `clang-format --dry-run --Werror` in CI, `clang-tidy`, `.editorconfig`,
   `scripts/format.*`, `scripts/lint.*`.
10. **One generated file pair per component** (`<Component>.g.h` / `.g.cpp`).

## Consequences

### Positive

- Predictable incremental builds and better cache hit rates.
- Review diffs focus on behavior, not formatting.
- Public headers do not drag Vulkan/third-party rebuilds into the UI.
- Generated output is stable and cache-friendly.

### Negative / risks

- More files and more discipline.
- clang-format/clang-tidy become build-time dependencies.
- A hard 500-line limit can feel arbitrary; it is a trigger to split along a real boundary,
  not a mechanical chop.

## Alternatives considered

- **Unity builds by default.** Rejected: faster cold build, worse incremental build and cache.
- **PCH by default.** Rejected: brittle, hides include hygiene, and invalidates broadly.
- **Single large translation units.** Rejected: poor cache locality and long rebuilds.
- **No formatter, style-by-review.** Rejected: inconsistent and wastes review time.

## Follow-up

- Measure `sccache`/`ccache` hit rates in CI.
- Add clang-tidy to CI once the codebase is warning-clean.
- Keep `scripts/format.*` and `scripts/lint.*` working on Windows and Linux.
