# ADR-038: C++23 language baseline

- Status: Accepted
- Date: 2026-11-08
- Supersedes: item 1 of [ADR-015](ADR-015-code-style-build-cache.md) (code style and build cache),
  which set the language standard to C++20.
- Related: [ADR-025](ADR-025-error-boundaries.md) (error boundaries and exception isolation),
  [ADR-012](ADR-012-static-linking.md) (static linking).

## Context

[AGENTS.md](../AGENTS.md) "Code conventions" requires `std::expected` for recoverable errors on hot
paths, and [`error-model-v1.md`](../docs/specs/error-model-v1.md) specified `std::expected<T, Error>`
with a project `StatusOr<T>` fallback for toolchains without it. `std::expected` is part of the
C++23 standard library, not C++20, so the project rule and the declared language standard
(C++20) contradicted each other (recorded as item U7 in
[`doc-contradiction-resolutions-v1.md`](../docs/architecture/doc-contradiction-resolutions-v1.md)).

## Decision

1. The project language standard is **C++23** for all Tessera code and for generated code.
2. `std::expected<T, Error>` is the error type for recoverable hot paths. The `StatusOr<T>`
   fallback is removed.
3. C++20 features already used (coroutines, concepts, `std::span`, `std::format`) remain valid and
   are unchanged; documents that say "C++20 coroutines" describe a feature introduced in C++20 and
   stay correct.
4. Build files declare the standard in one place per concern: `CMAKE_CXX_STANDARD` in
   `CMakeLists.txt` and `CMakePresets.json`, `cxx_std_23` in each target's
   `target_compile_features`, and `Standard: Latest` in `.clang-format` (the clang-format option
   `c++23` does not exist in the tool; `Latest` selects the newest standard it supports).
5. The `language` field of `tessera.json` accepts only `c++23` in V1
   ([`project-config-v1.md`](../docs/specs/project-config-v1.md)).
6. Every supported compiler must provide `std::expected`. The minimum compiler versions are not
   set by this ADR; CI is the check (the Linux job in `.github/workflows/ci.yml` and the Windows
   Visual Studio presets). A compiler that cannot build the tree is unsupported until the minimum
   versions are written in [`getting-started.md`](../docs/guides/getting-started.md).

## Consequences

- One error type, no fallback code path, no divergence between documents and the build.
- Older toolchains that lacked C++23 library support can no longer build Tessera.
- Visual Studio builds use the compiler's latest-standard switch, which MSVC treats as a moving
  target; CI on the `vs2026` and `vs2022` presets must keep passing.
- Third-party libraries that are statically linked must be buildable with C++23 or be built with
  their own standard setting.

## Alternatives

- Keep C++20 and a project `StatusOr<T>`: rejected; two error types, a fallback path to test, and
  a rule in AGENTS.md that cannot be met on all toolchains.
- Keep C++20 and change AGENTS.md to drop `std::expected`: rejected; the project owner chose C++23.

## Follow-up

- Update build files and documents listed in the decision (done in the same change).
- Write the minimum compiler versions in `getting-started.md` after a CI run on each toolchain.
- Configure and build on Windows (`vs2026` preset) and Linux to confirm the change.
