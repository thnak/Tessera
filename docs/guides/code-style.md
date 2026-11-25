# C++ code style and build-cache-friendly structure

This guide implements [ADR-015](../../decisions/ADR-015-code-style-build-cache.md) (code style and build cache). Project-wide rules are in [AGENTS.md](../../AGENTS.md).

The goal is a boringly consistent codebase that compiles incrementally and caches well.
Formatting and linting are enforced by tools, not by review comments. The formatter version is
pinned in [`tools/format-version.txt`](../../tools/format-version.txt) (currently `22.1.3`); CI
installs exactly that version and the `format-version` gate
(`python -B scripts/check_format_version.py`) fails loudly on any mismatch.

## Language baseline

- **C++23** for all shipped code ([ADR-038](../../decisions/ADR-038-cpp23-baseline.md)).
- No RTTI/reflection on hot paths (hot path = code that runs per frame or per input event). No exceptions in frame-critical code.
- A `std::function` may be **stored** (a registration or submission seam: `post`, `submit`,
  `schedule_*`, an event listener) but must not be **constructed or copied on a per-frame or
  per-node path**. Storing one is a one-time cost at registration; constructing one per call is a
  heap allocation on the frame path. Where a callback runs per event, prefer a function pointer plus
  opaque user data (`InputPump::GestureCallback` is the reference). See
  [the audit](../design/std-function-audit.md) for the measurement behind this rule.
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

## Object size and layout

Budgets here are proposals until `cpu_benchmark` confirms them; "Now" is measured (GCC 13.3, Linux x86-64, Release).

`struct` and `class` lay out and run identically; only default access differs. What changes speed is
how many bytes a pass touches per object, how they are aligned, and how many objects there are.

### Rules

| Rule | Guideline |
|---|---|
| Budget by multiplication | bytes a pass reads per object x instances, against the target cache (one core's L2 first, then LLC). 12 000 nodes x 64 B is 0.77 MB and fits L2; x 1.5 KB is 18 MB and does not. |
| One object per instance count | an object with thousands of instances (per node, per vertex, per record) carries a size budget and a `static_assert`. An object with a handful of instances (window, device, host) carries none; do not optimize it. |
| Hot part in the first 64 bytes | fields every frame pass reads go in the first cache line; everything else follows, or moves to a side struct reached by index. `ui::Node` is the model: 320 B total, hot fields on line 0. |
| Small arrays divide 64 | an element stored in a large array is 16, 32 or 64 B, so none straddles a line. 48 B and 88 B elements straddle; round or trim. |
| Alignment is part of the size | a 64 B struct only helps if it starts on a line (`detail::CacheLine<T>`, class-level aligned `operator new`, or an array of 64-multiples). |
| No heavy members inline | no `std::string`, `std::function`, `std::vector` or `std::map` directly in a per-node struct. Hold an index or pointer to a side table, or keep it only in the variant that needs it (text nodes, not every node). |
| Narrow where it is free | 32-bit index or handle instead of a pointer or `uint64_t` id when the table is bounded; bools and small enums packed together; members ordered by alignment so no padding appears. Skip bitfields on a hot read path if they add shifts that show in a profile. |
| Writers do not share a line | fields written by different threads go on different lines (false sharing, see [cache-lines](../design/cache-lines.md)). |
| Third-party types are budgeted, not edited | count them in the table (Yoga node), and justify any patch that grows them with a measurement, as the 16-slot Yoga cache patch did. |
| Every budget has a gate | `static_assert(sizeof(T) <= kTBudget)` for the type, plus a row in `tests/golden/perf/memory_budget.json` for types a header assert cannot see (third-party, MSVC layouts). A budget changes only in a commit that says why. |

### Proposed budgets (per-node and per-record types)

| Type | Now (B) | Target (B) | Plan |
|---|---|---|---|
| `ui::Node` | 320 | 320 | keep; exact assert stays |
| `layout::detail::RetainedNode` | 288 | 288 (no split) | measured on MSVC at 12 000 nodes: moving the text payload behind the hot fields (reorder only) moved nothing, so the split below is not taken: median one-text frame 9.56 vs 9.58 ms, layout engine 3.44 vs 3.42 ms, one-size frame 7.03 vs 6.88 ms (5 interleaved runs each). The retained tree is about 3.5 MB, and the layout engine's cost is Yoga, not these fields. Revisit only if a counter shows L1D or LLC misses on it |
| `layout::LayoutInput` | 96 (pinned; was 88) | 96 | trim to one line, or split what only text nodes read |
| `layout::LayoutBox` | 48 (pinned) | 32 or 64 | avoid straddling in arrays |
| `ui::PaintData` | 520 (heap, per painted node) | 520 (no split) | counted on the 12 000-node benchmark scene: 649 text runs and 9 images carry one, so about 658 x 520 B = 0.34 MB, under 3% of the 12 MB the per-node types cost, and it is allocated per painted node, not per node. By member: `gradient` 200, `decoration` 104, `vector` 56, `image` 56, `text` 32, `text_style` 28, `spans` 24; a plain text node uses about 80 B of the 520. Splitting the cold 416 B would save at most about 0.27 MB on this scene and add an indirection to every paint read, so it is not taken. Also measured: moving `wrap`, `max_lines` and `text_align` next to `kind` (so layout's input pass reads one line instead of two, and the struct drops to 512 B) left the layout-input phase at 1.86 vs 1.89 ms, so it was not kept. Revisit for a scene where most nodes paint a gradient, with a measurement |
| `style::ComputedStyle` | 16 | 16 | already a delta over shared defaults |
| `yoga::Node` (patched, 16 measurement slots) | 776 (584 upstream) | keep | the patch buys about 3.6 ms per frame at 12k nodes; revisit only with a new measurement |
| `Vertex2D`, `ClipRecord` | 64, 64 | 64 | already asserted |

Total per node today, without `PaintData`: Node + Yoga + RetainedNode + LayoutInput + LayoutBox = 1 520 B
(about 18 MB at 12 000 nodes). Report this sum in `cpu_benchmark` next to `sizeof_node`.

### Where the layout-input phase goes

Measured with the 12 000-node benchmark scene (MSVC Release, best of 30 fill passes, interleaved
variants that skip one step each): the whole phase was 1.83 ms. Skipping the `ComputedStyle` resolve
and assignment saved 0.1 ms, skipping the layout-signature fold 0.3 ms, skipping the text clear and
copy 0.7 ms, and skipping all three left 0.5 ms for the walk itself. Most leaves are text nodes, so
the text step is a dependent load through `unique_ptr<optional<PaintData>>` per node (about 70 ns),
not the string copy: the benchmark's strings fit the small-string buffer. A layout of `PaintData`
that put the fields it reads in one line changed nothing, so the cost was the indirection itself,
and no object-size change removes it.

What removed it was not reading unchanged nodes at all. The retained `LayoutInput` tree keeps each
subtree's signature (`subtree_signature`, 8 bytes, which is why `LayoutInput` is 96 B), the
signature is hierarchical so a skipped subtree contributes its stored value, and a pass keeps every
subtree with no style, layout or paint dirt under it (`src/app/layout_input_reuse.hpp`). The phase
fell from 1.85 ms to 0.01 ms, the one-text frame from 9.5 to 7.7 ms and the one-size frame from 6.9
to 5.0 ms. The signature of the partly reused tree equalled a full rebuild's on every frame of the
benchmark. A size budget bought here (8 B on a per-node record) is the trade the rules exist to make
visible.

### Checking a change

1. Print `sizeof` and the member offsets (`g++ -fdump-lang-class`, `pahole`, or MSVC `/d1reportSingleClassLayout`).
2. Run `tessera_benchmark --walk` and the one-text-change profile before and after. A size change that
   does not move a counter (L1D or LLC misses) or a time is kept only if it removes a structural cost.
3. Update the budget row and the asserts in the same commit.

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
- Every codegen change keeps the generated-base goldens and the `bind::` compile-fail tests green.

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
| Padding audit | `CMakeLists.txt` (`TESSERA_WARN_PADDING`) | `-Wpadded`/`/w14242`, off by default; see below |
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

- Header comment: `// ADR-040 generated base: do not edit. Developer code goes in
  <Component>.hpp/.cpp, derived from <Component>Base<Self>.`
- `clang-format` is applied with the repository style to generated text.
- Stable ordering: components sorted by path; nodes in source order; bindings sorted by id.
- One generated `<Component>.g.hpp` per component; no "generated everything" file.
- The generated base plus the developer class follow
  [ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md) exactly: the developer
  file is scaffolded once and never rewritten.
- No `SPDX-License-Identifier` forced into generated user code; the project's own license and
  `THIRD_PARTY_NOTICES` cover it. (Repository source files themselves carry
  `SPDX-License-Identifier: MIT`.)

## Definition of done

This list is the style-related subset of the full definition of done in [AGENTS.md](../../AGENTS.md#definition-of-done-every-change).

- `scripts/format.* --check` passes.
- Build with `TESSERA_WARNINGS_AS_ERRORS=ON`; fix warnings instead of adding blanket suppressions.
- Run the padding audit when you add a hot struct: `cmake --preset dev -DTESSERA_WARN_PADDING=ON
  -DTESSERA_WARNINGS_AS_ERRORS=OFF` and read the `-Wpadded` hits. It is off by default because the
  tree has 236 of them across 105 headers (issue #731), so it cannot be a gate; the types that were
  fixed are pinned by `padding_size_tests` and the memory budget instead. Order members
  widest-first: that removed 8 bytes each from `PaceDecision`, `AdaptiveQualitySample`,
  `OverlayPlacement` and `PaintData`. Tail padding is not removable by reordering.
- `clang-tidy` reports no new warnings in changed files.
- Files stay under the soft limit unless there is a documented reason (the hard limit of 500 lines
  has no exceptions except the Phase 0 PoC targets listed in AGENTS.md).
  `scripts/check_source_size.py` enforces this as part of `scripts/gates.py` (gate `source-size`,
  issue #327): a new file over 500 lines fails, and so does any file that *grows* past the count
  recorded for it in `scripts/source-size-baseline.txt`. Files that were already over the limit when
  the gate landed are grandfathered at that count and reported as warnings, so the only edit the
  baseline accepts is a lower number. After a real split, run
  `python -B scripts/check_source_size.py --update` to lower it (the script refuses to raise one) and
  delete the line once the file is under the limit.
- No new heavy include in a public header.
- A clean build and an incremental rebuild both work with the compiler cache.
