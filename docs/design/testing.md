# Testing strategy

Status: document current B1 tests and the target B2/B3/C/D test pyramid. ADR-009 is normative
for goldens.

## 1. Principles

- Tests are deterministic: no wall clock, network, system fonts, or random ids.
- Golden images come from a software path (lavapipe/CPU), never a real GPU.
- Every bug fix gets a regression test.
- Platform behavior is tested with synthetic event scripts, not only real devices.
- Error boundaries are tested with throwing build/update/effect/event/async/fallback cases.
- CI runs the same commands developers run locally.

## 2. Test pyramid

| Layer | What | Runs |
|---|---|---|
| Unit | pure logic, data structures, parsers, merger, layout math | every CI job |
| Integration | module seams (HAL -> UI -> renderer) | CI, fake HAL |
| Golden | image + structural report | lavapipe/software |
| Platform smoke | window/input/display on real OS | CI/device matrix |
| Performance | frame budgets, allocation counts, cache hit rates | nightly/benchmark |
| Fuzz | parser/CSS/IR/asset decoders | scheduled |

## 3. Current tests (B1)

| Test | Covers |
|---|---|
| `merger_unit` | protected-region extraction/merge/idempotency/CRLF/warnings |
| `rhi_null` | null device command stats, buffer round-trip |
| `scene_graph` | world bounds, deep hit testing, capture/target/bubble, stop propagation |
| `batched2d` | batching/vertex counts across texture/scissor changes |
| `batched2d_vulkan_smoke` | Vulkan 1.3 dynamic rendering smoke |
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
- Linux: X11 and Wayland separately; RandR/wl_output, configure/ack, input.
- Android/iOS/macOS later.
- Use synthetic events where possible; device smoke tests for real OS quirks.
- No platform-specific assumptions in unit tests; use a fake HAL.

## 7. Performance and memory

- Frame-time budgets per stage (style/layout/paint/render).
- Allocation counters: steady-state frame allocates zero after warm-up.
- Cache hit/miss/eviction metrics; atlas pages; GPU memory.
- Static UI idles: zero presents/submissions after settling.
- Benchmarks are explicit, opt-in, and not shipped in release.

## 8. Sanitizers and static analysis

- ASan/UBSan on unit/integration tests where supported.
- TSan for async/threading paths.
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

Current Linux CI:

1. install toolchain + lavapipe + `clang-format==22.1.3`
2. format check
3. configure/build (`ci` preset)
4. static C++ runtime check (`ldd`)
5. `ctest` with `VK_ICD_FILENAMES` pointing at lavapipe
6. offscreen Vulkan smoke + artifact upload

Windows CI should mirror: VS preset build, ctest, Vulkan smoke, format/lint, static link check.

## 12. Test conventions

- One test file per component; `tests/unit/<component>_tests.cpp`.
- Register in `tests/unit/CMakeLists.txt` with `add_test`.
- Tests return nonzero on failure and print a clear message.
- No test frameworks required in B1; a small framework may be introduced later.
- Fixtures live in `tests/fixtures/`; golden baselines in `tests/golden/baseline/`.
- Do not commit generated artifacts or machine-specific paths.

## 13. Status

- B1: unit + Vulkan smoke + merger CLI.
- B2: style/layout/text/input/loop/dirty/pacing/async/diagnostics tests; first goldens.
- B3: window/swapchain/DPI/lifecycle/OS-event tests; multi-frame sync.
- C: parser/IR/codegen/merger tests; generated-code compile tests.
- D: Wasm/IPC/hot-reload/parity tests.
