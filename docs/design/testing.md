# Testing strategy

Status: document current B1 tests and the target B2/B3/C/D test pyramid (B1-B3 are phases of Big Step B, C and D are Big Steps C and D; see [AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). [ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests) is normative
for goldens. Headless automation/agent harness: [`headless-automation-v1.md`](../specs/headless-automation-v1.md);
audits: [`lighthouse-audit-v1.md`](../specs/lighthouse-audit-v1.md).

## 1. Principles

- Tests are deterministic: no wall clock, network, system fonts, or random ids.
- Golden images (reference images, see [glossary](../reference/glossary.md)) come from a software path (lavapipe = Mesa software Vulkan driver, or the CPU rasterizer), never a real GPU.
- Every bug fix gets a regression test.
- Platform behavior is tested with synthetic event scripts, not only real devices.
- Error boundaries ([`error-boundaries-v1.md`](../specs/error-boundaries-v1.md)) are tested with throwing build/update/effect/event/async/fallback cases.
- CI runs the same commands developers run locally.

## 2. Test pyramid

| Layer | What | Runs |
|---|---|---|
| Unit | pure logic, data structures, parsers, merger, layout math | every CI job |
| Integration | module seams (HAL -> UI -> renderer; HAL = hardware/OS abstraction layer, [`hal.md`](hal.md)) | CI, fake HAL |
| Golden | image + structural report | lavapipe/software |
| Headless automation | scripts/actions/snapshots/traces/audits | null/software/lavapipe |
| Interaction suites | text editing/IME, scroll/gestures, overlays, navigation, forms, a11y | headless + device matrix |
| Platform smoke | window/input/display on real OS | CI/device matrix |
| Performance | frame budgets, allocation counts, cache hit rates | nightly/benchmark |
| Fuzz | parser/CSS/IR/asset decoders | scheduled |

## 3. Current tests

| Test | Covers |
|---|---|
| `merger_unit` | protected-region merger (keeps developer C++ across regeneration) extraction/merge/idempotency/CRLF/warnings |
| `rhi_null` | null device command stats, buffer round-trip |
| `scene_graph` | world bounds, deep hit testing, capture/target/bubble, stop propagation |
| `scene_dirty` | dirty domains, ancestor aggregation, clear/recompute, setter invalidation |
| `dirty_pass` | pruned collect by domain, parent-first order, clear, node cap |
| `fixed_step` | deterministic accumulator, delta clamp, catch-up cap, time scale |
| `clock` | deterministic manual clock, monotonic advance validation |
| `runtime_loop` | clock-driven steps, one-shot requests, dirty persistence, continuous/on-demand modes |
| `runtime_loop_async` | completion budget, pending wake, callback frame/dirty requests, no-queue path |
| `frame_pacer` | uncapped/deadline waits, lateness, cadence reset, validation |
| `async` | cancellation propagation, FIFO/nested posting, bounded overflow, skip cancelled, budget |
| `completion_queue` | visible/layout/background priority, FIFO, per-tick budget, cancellation, overflow |
| `input` | bounded queue FIFO/overflow, modifier flags, adjacent-move coalescing |
| `damage` | clip, overlap/touch merge, margin, cap/full-redraw fallback |
| `redraw_plan` | integer edge rounding, target clipping, full-redraw plan, determinism |
| `tokenizer` | CSS tokens, offsets, escapes, strings, numbers, functions, match tokens, ranges |
| `parser` | rule/declaration/at-rule AST, `!important`, custom properties, tolerant recovery |
| `selector` | combinators, attributes, pseudos/functions, robustness, specificity |
| `selector_match` | right-to-left matching, combinators, attributes, state/structural pseudos, index |
| `cascade` | origin/importance/specificity/source order, property normalization, inheritance keywords |
| `compiled_stylesheet` | compile rules, selector list specificity, cascade composition, inheritance, recovery |
| `property_registry` | metadata, box/pair/flex expansion, diagnostics, initial values |
| `media_query` | width/height/resolution, orientation, color scheme, reduced motion, lists/not |
| `keyframes` | from/to/percent offsets, comma selectors, invalid steps |
| `supports` | property/value support, custom properties, not/and/or precedence and nesting |
| `layout` | flex row/column, grow, gap/padding, absolute, percent/min/max, ratio, RTL, wrap, text measure, determinism |
| `layout_cache` | repeated layout hits text cache, same boxes, capacity injection, changed text refetch |
| `text_measure` | stub advance, word wrap, long-word break, newlines, letter spacing, style parsing |
| `text_measure_cache` | key dimensions, LRU eviction, hit/miss stats, invalidate/clear, capacity guards |
| `layout_apply` | id matching, parent-relative bounds, dirty propagation, missing/extra boxes |
| `pixel_snap` | half-up/negative symmetry, scales 1/1.25/2, clamp below 1, joined edges |
| `damage_pass` | Paint/Render filtering, snapped world bounds, merge, DPI scale, cap/full-redraw, overflow |
| `damage_tracker` | old+new bounds, unchanged dirty, prune removed, retention eviction, capacity guard |
| `animation` | linear/unit/discrete interpolation, delay/fill, reverse/alternate, infinite iterations |
| `animation_runtime` | start/tick/finish, callbacks, stop/clear, wake state, dirty domains |
| `easing` | linear/presets/bezier identity/steps variants, parser rejection, sampler integration |
| `spring` | exact semi-implicit step, convergence/epsilon stop, retarget, invalid config, determinism |
| `batched2d` | batching/vertex counts across texture/scissor changes |
| `batched2d_vulkan_smoke` | Vulkan 1.3 dynamic rendering smoke |
| `batched2d_vulkan_validation` | Vulkan render with validation layers |
| `merger_cli_help`, `merger_cli_idempotent` | merger CLI behavior |

Run:

```bash
ctest --preset dev
ctest --preset ci
```

## 4. Golden tests

- Fixed offscreen size, DPI/scale, fonts, clock, and inputs.
- Bit-exact comparison on the software path; reviewed `--update-goldens` only.
- GPU runs are smoke tests with tolerance; they never update baselines.
- Each failure emits an image diff + `report.json` with structural data.
- See [`golden-tests-v1.md`](../specs/golden-tests-v1.md).

## 5. Deterministic mode

- Simulated monotonic clock and fixed timestep.
- Input/OS event scripts (resize, DPI, fold, power, IME).
- Fixed locale/time zone/ICU data.
- Committed fonts and assets; no system lookups.
- Single-threaded/deterministic executor; no detached tasks.
- No network; filesystem access restricted to fixtures.

## 6. Platform tests

- Windows: Win32 message loop, DPI, resize, power, display topology.
- Linux: X11 and Wayland separately; RandR (X11 display-configuration extension)/wl_output (Wayland display object), configure/ack (Wayland resize handshake), input.
- Android/iOS/macOS: Big Step F (production breadth), no platform tests yet.
- Use synthetic events where possible; device smoke tests for real OS quirks.
- No platform-specific assumptions in unit tests; use a fake HAL.

## 7. Performance and memory

- Frame-time budgets per stage (style/layout/paint/render).
- Allocation counters: steady-state frame allocates zero after warm-up.
- Cache hit/miss/eviction metrics; atlas pages; GPU memory.
- Static UI idles: zero presents/submissions after settling.
- Benchmarks are explicit, opt-in, and not shipped in release.

## 8. Sanitizers and static analysis

- ASan (AddressSanitizer)/UBSan (UndefinedBehaviorSanitizer) on unit/integration tests on toolchains that support them (the supported list is not specified).
- TSan (ThreadSanitizer) for async/threading paths.
- Validation layers for Vulkan debug builds (mandatory).
- clang-tidy/format checks in CI.
- Static link graph check (no unexpected dynamic third-party libs).

## 9. Fuzzing

- CSS tokenizer/parser, JSX/TSX parser adapter, IR validation, bindings manifest.
- Image/asset decoders with size limits.
- IPC message parser (designer).
- Bounded corpora and timeouts; crashes are failures.

## 10. AI-agent feedback

Golden failures produce:

- `report.json`: node ids, bounds, colors, dirty reasons, diff regions
- image diff
- reproduction command + seed/event script

This is the structured feedback channel for automated visual regression fixes.

## 11. CI

CI matrix (`.github/workflows/ci.yml`):

| Job | Toolchain | Coverage |
|---|---|---|
| Linux | GCC + Ninja + Vulkan SDK/lavapipe | format, full build with `TESSERA_WARNINGS_AS_ERRORS=ON`, null tests, Vulkan smoke + validation, static-runtime check, offscreen artifact |
| Windows | MSVC + VS2022 | null/offscreen build with `TESSERA_WARNINGS_AS_ERRORS=ON`, tests |
| macOS | AppleClang | null/offscreen build with `TESSERA_WARNINGS_AS_ERRORS=ON`, tests |

Linux is the full GPU gate (lavapipe is the deterministic software Vulkan driver); Windows/macOS
gate portable compilation and the null/offscreen paths until their Vulkan SDK setup is added.
Real-GPU runs are local/nightly smoke only, never golden baselines.

## 12. Test conventions

- UI/compiler unit tests: `tests/unit/<component>_tests.cpp`, registered in
  `tests/unit/CMakeLists.txt`.
- GPU tests: `tests/gpu/<area>/`, registered in `tests/gpu/CMakeLists.txt`; see
  [`gpu/testing.md`](gpu/testing.md).
- Tests return nonzero on failure and print a clear message.
- No test framework is used in B1; whether to adopt one has no decision yet.
- Fixtures live in `tests/fixtures/` (GPU: `tests/fixtures/gpu/`); golden baselines in
  `tests/golden/` (GPU: `tests/golden/gpu/`).
- Do not commit generated artifacts or machine-specific paths.

### 12.1 GPU test tree

```text
tests/gpu/{rhi,resources,pipelines,frames,backends,shaders,batched2d}/
tests/integration/gpu/
tests/golden/gpu/
tests/fixtures/gpu/
tests/benchmarks/gpu/
```

New GPU tests go under `tests/gpu`; do not add them to `tests/unit`. The null backend covers
command/validation structure; lavapipe/software covers pixels; real GPUs are smoke/tolerance only.

## 13. Status

- B1: unit + Vulkan smoke + merger CLI.
- B2: style/layout/text/input/loop/dirty/pacing/async/diagnostics tests; first goldens.
- B3: window/swapchain/DPI/lifecycle/OS-event tests; multi-frame sync.
- C: parser/IR/codegen/merger tests; generated-code compile tests.
- D: Wasm/IPC/hot-reload/parity tests.
