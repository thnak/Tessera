# Testing guide

Status: current B1 tests run with CTest; golden tests are B2/B3.

## 1. Run the tests

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
```

Linux CI uses lavapipe:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ctest --preset ci
```

## 2. Current tests

| Test | What it covers |
|---|---|
| `merger_unit` | protected-region extraction/merge/idempotency/CRLF/warnings |
| `rhi_null` | null device commands, buffer round-trip |
| `scene_graph` | hit testing, capture/target/bubble, stop propagation |
| `batched2d` | batch/vertex counts on the null backend |
| `batched2d_vulkan_smoke` | Vulkan 1.3 smoke render |
| `merger_cli_help` | merger CLI usage |
| `merger_cli_idempotent` | CLI merge idempotency |

## 3. Add a unit test

1. Create `tests/unit/<component>_tests.cpp`.
2. Use the existing style: a `main()` returning nonzero on failure; print a clear message.
3. Add to `tests/unit/CMakeLists.txt`:

```cmake
add_executable(my_component_tests my_component_tests.cpp)
target_link_libraries(my_component_tests PRIVATE tessera_my_component)
add_test(NAME my_component COMMAND my_component_tests)
```

4. Keep tests deterministic: no wall clock, network, system fonts, or random ids.
5. Run `ctest -R my_component --output-on-failure`.

## 3.0 Headless agent tests

```bash
tessera test --script tests/agent/home.json --report out/report.json
tessera test --script tests/agent/scroll.json --mode golden --out out/
tessera headless --mode audit --profile desktop --script tests/audit/home.json
```

- Use numeric `uiId`/`(uiId,itemKey)` from `generated/automation.manifest.json`, not coordinates.
- Structural mode (null backend) is fast; golden mode uses software/lavapipe.
- Assertions: visibility/text/focus/bounds/events/no boundary catches/golden/audit thresholds.
- Outputs: `report.json`, `snapshot.json`, `trace.json`, `audit.json`.
- See [`agent-automation.md`](agent-automation.md) and
  [`headless-automation-v1.md`](../specs/headless-automation-v1.md).

## 3.1 Error-boundary tests

- Throw in child build/update/effect/event -> fallback shown, error logged.
- Nested boundary: nearest catches; fallback failure propagates to parent/root.
- Reset/retry/resetKey; async error report; cancellation does not show fallback.
- No silent catch; redaction; no throw across threads/FFI; loop protection.

## 4. Golden tests (target)

- Use the software/lavapipe path, fixed size/scale/fonts/clock/events.
- Compare bit-exact; update baselines only with an explicit reviewed command.
- Failure artifacts: image diff + `report.json` (node bounds/colors/dirty).
- See [`golden-tests-v1.md`](../specs/golden-tests-v1.md) and
  [`design/testing.md`](../design/testing.md).

## 5. Deterministic mode

- Fixed timestep and simulated clock.
- Synthetic input/OS events (resize, DPI, fold, power, IME).
- Fixed locale/time zone/ICU data.
- Committed fonts/assets; no system lookups.
- Single-threaded/deterministic executor; no detached tasks.
- No network; filesystem limited to fixtures.

## 6. Sanitizers and validation

- ASan/UBSan: unit/integration tests where supported.
- TSan: async/threading paths.
- Vulkan validation layers: mandatory in debug; CI may disable only if lavapipe cannot provide
  the layer.
- clang-tidy/format: CI gates.

## 7. Benchmarks

- Benchmarks are opt-in and not shipped.
- Measure frame stages (style/layout/paint/render), allocation counts, cache hit rates, GPU
  memory.
- A static scene must produce zero presents/submissions after settling.
- Record baselines and compare with tolerance; do not gate on noisy wall-clock numbers.

## 8. Fuzzing

- CSS tokenizer/parser, JSX/TSX parser, IR validation, manifest/IPC parsers, asset decoders.
- Bounded inputs/timeouts; crashes are failures.
- Run scheduled, not on every PR.

## 9. CI

Current Linux workflow:

1. install toolchain + lavapipe + `clang-format==22.1.3`
2. format check
3. configure/build (`ci` preset)
4. static C++ runtime check
5. ctest with lavapipe
6. Vulkan offscreen smoke + artifact

Windows CI should mirror these steps with the VS preset.

## 10. Failure triage

- Reproduce with the exact preset/backend/ICD.
- For GPU-only failures, run with validation layers and lavapipe.
- For async failures, run TSan and reduce to a deterministic script.
- For golden failures, inspect `report.json` + diff before updating anything.
- Add a regression test before/with the fix.
