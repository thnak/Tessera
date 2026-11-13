# GPU testing

Status: B1 GPU tests exist (`rhi_null`, `batched2d`, Vulkan smoke); B2 goldens and the contract
suite are partially implemented (lavapipe and CPU goldens, damage equivalence, frames in flight,
Vulkan swapchain/async tests under `tests/gpu/`), and the cross-backend image parity harness
(section 5, issue #245) renders one fixed scene set on every available backend arm. Overall test
strategy: [`../testing.md`](../testing.md);
golden format: [`../../specs/golden-tests-v1.md`](../../specs/golden-tests-v1.md).

## 1. Goals

- Catch RHI/resource/command bugs without a real GPU.
- Keep goldens deterministic (software/lavapipe only).
- Exercise Vulkan/WebGPU parity through one logical contract and one image metric.
- Bound resource lifetime/frames-in-flight bugs with fake completion, not sleeps.
- Keep CI fast: null/unit on every job; GPU smoke optional.

## 2. Test pyramid

| Layer | What | Runs |
|---|---|---|
| Unit | descriptors, cache keys, slot math, batch keys, validation | every CI |
| RHI contract | same command script on null/lavapipe/Vulkan/WebGPU | every CI (null); GPU job |
| Integration | upload -> draw -> readback; resize; frames in flight | CI/software |
| Golden | fixed offscreen images/structural reports | software/lavapipe |
| Cross-backend parity | fixed scene set on every arm vs lavapipe, SSIM/deltaE + classes | every CI (arms named when skipped) |
| GPU smoke | real device, tolerance, no baselines; validation layers when available | optional/nightly |
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
    backends/            # cross-backend image parity harness (issue #245)
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

## 5. Cross-backend image parity (issue #245)

The RHI contract suite proves that every backend accepts the same commands; the parity harness in
`tests/gpu/backends/` proves that they produce the same picture. It renders one fixed scene set
through the batched 2D renderer over the RHI on every arm the build and the host offer, and
compares each arm's readback against the lavapipe reference (ADR-009). It is the testing half of
the M4 acceptance ("the same screen previews in a browser and matches native output") and the check
that the Metal backend (ADR-053) will be held to.

- **Scene set** (`parity_scenes.hpp`): eight 128x96 RGBA8 UNORM scenes - solid quad, SDF rounded
  quad, textured quads, rectangular clip, rotation transform, text (procedural glyph-atlas quads),
  stacked-layer shadow, alpha blend. AA off and golden mode, fixed clear colour, no clock, no
  random ids, no system fonts: only the rasterizer may differ.
- **Arms** (`parity_backend.hpp`): `lavapipe` (reference), `vulkan` (host device), `webgpu`,
  `metal` (reserved, ADR-053) and `null` (command counters only). An unavailable arm prints
  `backend '<name>': skipped (<reason>)` and the ctest reference entry reports a skip; nothing is
  silently green.
- **Reference images**: `gpu_backend_parity --write-reference` runs only when the process selected
  the lavapipe ICD and writes `parity/reference/<scene>.png` under the test binary directory. The
  parity ctest entry (`gpu_backend_parity`) depends on it, then renders every candidate arm and
  compares pair `<candidate>_vs_lavapipe`.
- **Metric and classes**: SSIM on Rec.709 luma, CIE76 deltaE in CIELAB, and the difference classes
  (edge rounding vs structural, with the worst region) in `rendering/golden/`; the spec
  (`../../specs/golden-tests-v1.md` section 4) is normative for the formulas.
- **Tolerances**: per-pair, from `tests/golden/tolerance.json` schema 2, generated into a C++
  table at configure time by `scripts/golden_tolerances.py`. An unlisted pair is exact, so adding a
  backend does not silently loosen anything.
- **Determinism first**: every pixel arm is rendered twice and must be byte-identical; a difference
  fails as nondeterminism before the perceptual comparison.
- **Artifacts**: a failing pair writes `parity/diff/<pair>_<scene>.diff.png` (`make_diff_image`) and
  `parity/parity-report.json` (per-arm state, policy, metrics, counts, worst region, artifact
  paths); both paths are printed.

The null arm is the structural half: the same scenes must record the same `draw_calls` and
`vertices` counts as the pixel reference, which catches batching/command changes that never reach a
rasterizer.

## 6. Golden and GPU smoke

- Golden: fixed size/DPI/fonts/clock/inputs; software path; bit-exact; reviewed updates only.
- `--golden` disables AA where needed; fixed clear color/format.
- GPU smoke: tolerance-based, no baseline updates, may skip if no device.
- Each failure emits image diff + `report.json` (ids, bounds, colors, dirty reasons, diff).
- Never compare a real-GPU image to a software baseline bit-exactly.

## 7. Determinism

- No wall clock, network, system fonts, random ids, or driver defaults.
- Fake frame completion values; no `sleep`/busy-wait.
- Fixed resource/cache insertion order.
- Fixed shader/pipeline cache keys; no addresses/timestamps.
- Single-threaded/deterministic executor for golden runs.
- Device/backend/feature caps are injected, not queried differently per machine.
- Every cross-backend parity arm is rendered twice and must be byte-identical; a difference is a
  nondeterminism failure, not a tolerance question.

## 8. Commands

```bash
ctest --preset dev
ctest --preset dev -R 'rhi_|batched2d|gpu'
ctest --preset dev -R 'gpu_backend_parity|perceptual_metric|difference_class|parity_policy'
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ctest --preset ci
```

Windows:

```powershell
ctest --preset vs2026-release -R 'rhi_|batched2d|gpu'
```

Golden update is explicit and reviewed; GPU smoke never updates baselines. The parity reference
images are regenerated by `gpu_backend_parity --write-reference` on a lavapipe host only (the
`gpu_backend_parity_reference` ctest entry does this automatically when CMake finds the ICD).

## 9. Adding a GPU test

1. Put it under the matching `tests/gpu/<area>/` directory.
2. Add an executable and `add_test` in `tests/gpu/CMakeLists.txt`.
3. Use null for structure, lavapipe for pixels; inject limits/completion.
4. Add a golden only if the visual is deterministic; otherwise structural stats.
5. Keep runtime bounded; no large textures/frames in unit tests.
6. Document the failure message and reproduction command.
7. A new backend also adds a parity arm (`parity_backend.cpp`) and, if it is expected to differ at
   all from lavapipe, a `<backend>_vs_lavapipe` entry in `tests/golden/tolerance.json`; an
   unlisted pair is exact on purpose.

## 10. Triage

- Null failure: command order/descriptor/lifetime bug; fix before GPU.
- Lavapipe/golden failure: inspect diff + `report.json`; check shader/layout/AA/format.
- Real-GPU-only failure: capability/validation/driver bug; add a capability check/fallback.
- Cross-backend parity failure: read `parity-report.json`; a structural class names the offending
  region and is never a tolerance problem, while edge rounding inside the pair budget is expected
  and reports SSIM/deltaE.
- Nondeterministic parity render: look for uninitialized state or frame overlap before touching a
  tolerance.
- Flaky: suspect uninitialized state, frame overlap, or wall-clock; remove nondeterminism.
- Validation error: bug, not a warning; fail debug/CI.

## 11. Deferred/open

- Test framework choice (B1 uses `main()` returning nonzero).
- Automated golden review UI.
- WebGPU/browser test harness: the parity and conformance arms can select WebGPU, but they report
  a named skip until a host supplies a `WGPUDevice` (the `webgpu.yml` lane is its build check).
- Real-device CI matrix.
- GPU memory/leak stress thresholds.
