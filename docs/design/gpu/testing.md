# GPU testing

Status: B1 GPU tests exist (`rhi_null`, `batched2d`, Vulkan smoke); B2 goldens/contract suite
proposed. Overall test strategy: [`../testing.md`](../testing.md); golden format:
[`../../specs/golden-tests-v1.md`](../../specs/golden-tests-v1.md).

## 1. Goals

- Catch RHI/resource/command bugs without a real GPU.
- Keep goldens deterministic (software/lavapipe only).
- Exercise Vulkan/WebGPU parity through one logical contract.
- Bound resource lifetime/frames-in-flight bugs with fake completion, not sleeps.
- Keep CI fast: null/unit on every job; GPU smoke optional.

## 2. Test pyramid

| Layer | What | Runs |
|---|---|---|
| Unit | descriptors, cache keys, slot math, batch keys, validation | every CI |
| RHI contract | same command script on null/lavapipe/Vulkan | every CI (null); GPU job |
| Integration | upload -> draw -> readback; resize; frames in flight | CI/software |
| Golden | fixed offscreen images/structural reports | software/lavapipe |
| GPU smoke | real device, tolerance, no baselines | optional/nightly |
| Stress/bench | resource churn, upload ring, frames in flight | nightly |

## 3. Directory tree

```text
tests/
  unit/
    merger_tests.cpp
    scene_graph_tests.cpp
  gpu/
    README.md
    rhi/                 # device/command contract, null stats
    resources/           # buffers/textures/targets/cache/lifetime
    pipelines/           # cache key, layout, manifest/reflection
    frames/              # slots, upload/readback, resize, device loss
    backends/            # capability/parity/validation
    shaders/             # shader smoke helpers
  integration/
    gpu/                 # end-to-end GPU paths
  golden/
    gpu/                 # baselines (software/lavapipe only)
  fixtures/
    gpu/                 # shaders, small images, fake limits
  benchmarks/
    gpu/                 # frame/upload/alloc benchmarks
```

Rules:

- New GPU tests go under `tests/gpu/<area>/`; do not add them to `tests/unit/`. B1 GPU tests are
  already under `tests/gpu/rhi/` and `tests/gpu/batched2d/`.
- Register targets/tests in `tests/gpu/CMakeLists.txt`.
- Goldens live under `tests/golden/gpu/`; no machine-specific output.
- Fixtures are small, committed, license-clear, and deterministic.

## 4. RHI contract suite

One logical script runs against every backend:

```text
create device -> buffer -> texture -> target -> pipeline -> command list
  -> begin pass -> bind pipeline/buffer/texture -> viewport/scissor
  -> push constants -> draw -> end pass -> copy target -> submit
  -> readback -> compare expected stats/pixels
```

Assertions:

- invalid order/state fails (begin/end balance, pipeline before draw, push size)
- bind/draw/copy counts match
- buffer round-trip and readback dimensions/pitch
- resource destruction order is valid
- same logical output on null stats and software pixels

The null backend checks structure; lavapipe checks pixels; real Vulkan checks API/validation.

## 5. Golden and GPU smoke

- Golden: fixed size/DPI/fonts/clock/inputs; software path; bit-exact; reviewed updates only.
- `--golden` disables AA where needed; fixed clear color/format.
- GPU smoke: tolerance-based, no baseline updates, may skip if no device.
- Each failure emits image diff + `report.json` (ids, bounds, colors, dirty reasons, diff).
- Never compare a real-GPU image to a software baseline bit-exactly.

## 6. Determinism

- No wall clock, network, system fonts, random ids, or driver defaults.
- Fake frame completion values; no `sleep`/busy-wait.
- Fixed resource/cache insertion order.
- Fixed shader/pipeline cache keys; no addresses/timestamps.
- Single-threaded/deterministic executor for golden runs.
- Device/backend/feature caps are injected, not queried differently per machine.

## 7. Commands

```bash
ctest --preset dev
ctest --preset dev -R 'rhi_|batched2d|gpu'
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ctest --preset ci
```

Windows:

```powershell
ctest --preset vs2026-release -R 'rhi_|batched2d|gpu'
```

Golden update is explicit and reviewed; GPU smoke never updates baselines.

## 8. Adding a GPU test

1. Put it under the matching `tests/gpu/<area>/` directory.
2. Add an executable and `add_test` in `tests/gpu/CMakeLists.txt`.
3. Use null for structure, lavapipe for pixels; inject limits/completion.
4. Add a golden only if the visual is deterministic; otherwise structural stats.
5. Keep runtime bounded; no large textures/frames in unit tests.
6. Document the failure message and reproduction command.

## 9. Triage

- Null failure: command order/descriptor/lifetime bug; fix before GPU.
- Lavapipe/golden failure: inspect diff + `report.json`; check shader/layout/AA/format.
- Real-GPU-only failure: capability/validation/driver bug; add a capability check/fallback.
- Flaky: suspect uninitialized state, frame overlap, or wall-clock; remove nondeterminism.
- Validation error: bug, not a warning; fail debug/CI.

## 10. Deferred/open

- Test framework choice (B1 uses `main()` returning nonzero).
- Automated golden review UI.
- WebGPU/browser test harness.
- Real-device CI matrix.
- GPU memory/leak stress thresholds.
