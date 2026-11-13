# Golden tests v1

Status: partially implemented (Big Steps B2 and B3, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big
Steps"). The batched2d harness compares a lavapipe baseline through
`rendering/golden/image_diff.hpp` and `batched2d_poc --compare`; `--report`/`--diff` artifacts
exist, alongside a CPU-canvas solid-rect path; the wider AA/text/texture corpus is still
growing, and the cross-backend image parity harness (section 1.3) renders the same scene set on
every available RHI backend with a perceptual metric (section 4). Normative for visual regression;
[ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md) (deterministic
headless golden tests on a software rasterization path) is the decision.
Headless runner/agent protocol: [`headless-automation-v1.md`](headless-automation-v1.md).
Custom element goldens: [`custom-elements-v1.md`](custom-elements-v1.md).

## 1. Rules

1. Golden images (deterministic reference images; see
   [glossary](../reference/glossary.md)) come from a deterministic software path, never a real
   GPU framebuffer. The software path is either the Tessera CPU rasterizer (`software` backend)
   or Mesa lavapipe (a software Vulkan driver, ICD), per ADR-009.
2. Bit-exact comparison on the software path; any diff fails unless explicitly updated.
3. GPU runs are smoke tests with perceptual tolerance; they do not produce/update baselines.
4. Fixed clock, size, DPI, fonts, locale, inputs, and event scripts.
5. Golden updates are a separate, reviewed change.
6. Failures emit an image diff plus structural `report.json` for humans/agents.
7. Custom element goldens use compiled shaders, an offscreen target, fixed time/assets/input;
   `continuous` elements use a fixed frame script, not wall clock.

Implementation note: the first harness covers batched2d only. The baseline
`tests/golden/baseline/batched2d/dashboard.png` is generated with lavapipe, and the
`batched2d_golden` CTest compares it bit-exactly through `rendering/golden/image_diff.hpp` and
`batched2d_poc --compare`. Text-bearing cases (`batched2d_overlay_golden`,
`batched2d_text_golden`) allow a 1-level channel delta and a 0.1% pixel budget because glyph
antialiasing rounds differently across lavapipe/LLVM revisions. The corpus also covers a
shapes-only scene (`dashboard_shapes.png`: rounded cards, two texture UV scales, scissor clipping,
translation, and clip+transform) compared bit-exactly. `--report` and `--diff` write
structural/visual artifacts.

### 1.1 Harness frame-capture baseline

`harness_frame_capture` (`tests/gpu/harness_frame_capture_tests.cpp`) renders a small fixture scene
through the same offscreen `capture_scene_frame` path the CLI golden mode uses. In golden mode it
compares the capture against the committed baseline
`tests/golden/baseline/harness/fixture_golden.png`, which is generated on lavapipe with
`harness_frame_capture_tests --update-goldens` on a software device only (ADR-009: a gpu-smoke run
never produces or updates a baseline). The comparison allows a 1-level channel delta and a 0.1%
pixel budget because the fixture paints text and glyph antialiasing rounds differently across
lavapipe/LLVM revisions; the test skips on a host without a software device. gpu-smoke mode has no
committed baseline (any hardware device would make it non-portable) and only proves the check path
against a baseline made from the same run.

### 1.2 Software (CPU canvas) corpus

`batched2d_poc --backend cpu` renders through `CpuCanvas` (`src/rendering/software/`), the
software reference: alpha-blended axis-aligned solid quads with hardware-style pixel-center edges,
an 8-bit coverage blend for glyph bitmaps, and an integer nearest-sample textured-quad blit with
tint and a per-call clip. Its committed baselines run on every platform (no GPU, no lavapipe):
`dashboard_cpu.png` (solid rects), `dashboard_text_cpu.png` (coverage text),
`dashboard_vector_cpu.png`, and `dashboard_image_cpu.png` (textured quads).

The wider paint corpus is committed as `tests/golden/baseline/cpu_wide/{borders,clips,gradients}.png`
and rendered by `tests/gpu/cpu_wide_golden_tests.cpp` (ctest `cpu_wide_golden`) through
`ui::paint::paint_scene` and the CPU paint sink. Coverage:

- `borders`: one rounded card with four border sides of distinct widths and colours (the top,
  right, bottom and left edge quads), a second card with a uniform square border, and a borderless
  rounded card that pins the sink's documented bounds approximation (corners stay square);
- `clips`: three nested `overflow: hidden` scopes whose intersection cuts the fills, an outermost
  clip whose rect starts off-canvas, and a disjoint clip whose child must paint nothing even where
  the child would fall on the canvas;
- `gradients`: a vertical three-stop and a horizontal two-stop linear ramp sampled through
  `ui::paint::sample_linear_gradient` (the sink's shared 64-band encoded-space approximation, not a
  private copy of the ramp math), plus a gradient clipped by two nested scopes.

The three baselines are generated on the CPU path only with
`cpu_wide_golden_tests --update-goldens` and are bit-exact on Windows and Linux. The comparison
rule is exact: decode both PNGs to RGBA8, require equal dimensions, compare every byte including
alpha, and fail on a single differing pixel (per-channel tolerance 0, mismatch budget 0). No
libm/locale/font/GPU rounding is involved, so the same bytes must come out on every platform; the
test also renders every scene twice and requires byte identity before touching the baseline.

The deterministic corpus is also pinned without baseline PNGs, on exact RGBA8 bytes, by
`cpu_canvas_corpus` and `cpu_canvas_scene_corpus` (`tests/unit/`). Coverage:

- solid quads and the pixel-center edge rule (integral, fractional, half-pixel, 1-pixel);
- overlapping translucent quads (later draws win, so order is significant) and stacked coverage;
- textured quads: nearest-sample mapping, tint/alpha composition, clip, negative origins,
  degenerate quads;
- boundary geometry: zero-size, off-canvas, negative coordinates, 0x0 and 1x1 canvases;
- sink-level clip stacks: nested `overflow` clips (three-way intersection) and disjoint clips
  (an empty intersection must paint nothing);
- translate-only transforms (exact: the paint pass folds them into world bounds) and rounded-rect
  backgrounds;
- damage-scissored redraws: a full paint and a `ClippedPaintSink` replay per disjoint scissor
  region are byte-identical, with an undersized-region negative control.

Documented gaps of the software backend (deliberate, never faked; pinned by the corpus or kept on
the lavapipe side):

- no rounded/SDF rasterizer: `fill_rounded_rect` paints the axis-aligned bounds, so corners stay
  square;
- no rotation/scale/skew rasterizer: `PaintSink::supports_transform()` is false for the CPU sinks,
  so a transformed node paints its world-bounds AABB (translate is exact);
- no gradient rasterizer: gradients are approximated by 64 dominant-axis bands in the CPU sink;
- box shadows are stacked rect layers (3-8 per blurred shadow), not a Gaussian blur;
- `CpuCanvas` has no clip stack of its own: clipping is a per-call rect applied by the sink;
- nearest-sample only: vector/image draws are exact when the raster extent equals the destination
  box, and there is no resampling filter;
- no antialiasing on geometric edges (glyph coverage is the exception).

Those shapes remain on the lavapipe corpus (`dashboard_shapes.png`, text cases with tolerance),
which stays the full-fidelity software path.

### 1.2 Harness frame check (`--mode golden` / `--mode gpu-smoke` core)

`include/tessera/harness/{frame_capture,frame_check,image_compare}.hpp` (`src/harness/`) hold the
harness-side core; the CLI does not call it yet.

- `capture_scene_frame` renders one frame offscreen (no swapchain) through `app::capture_frame`
  and returns RGBA8. `check_frame` writes `frame.png`, compares it to a baseline PNG, and writes
  `frame.diff.png` (on mismatch) and `frame.compare.json` through the `ArtifactSink`.
- Golden policy is exact (0 channel delta, 0 mismatched pixels); gpu-smoke uses a per-channel
  tolerance (8) and a 2% pixel budget. SSIM/deltaE is not implemented; the budget approximates it.
- Golden requires the software path. The RHI does not report device type, so
  `capture_capability` accepts golden only when `VK_ICD_FILENAMES`/`VK_DRIVER_FILES` names only
  lavapipe; otherwise it returns a typed `NoSoftwareDevice` error (unavailable, not a failure).
- gpu-smoke never writes a baseline: `update_baseline` with it is rejected before anything is
  written. Golden updates only through `FrameCheckRequest::update_baseline`; the
  `--update-goldens` CLI/env exposure (ADR-009) is a later slice.

### 1.3 Cross-backend image parity harness (issue #245)

`tests/gpu/backends/` renders one fixed scene set through the RHI on every backend arm the build
and the host can provide, and compares each arm against the software reference. This is the M4
"same screen previews in a browser and matches native output" check, and the check the Metal
backend ([ADR-053](../../decisions/ADR-053-native-metal-backend.md)) will be held to.

- The scene set is eight small scenes at a fixed 128x96 RGBA8 UNORM extent: a solid quad, an SDF
  rounded quad, textured quads (1:1 checkerboard plus a 3x nearest ramp), a rectangular clip, a
  rotation transform, a text row (procedural glyph-atlas quads), a stacked-layer shadow and an
  alpha blend. Anti-aliasing is off and the renderer runs in golden mode, so the comparison is
  about rasterization, not coverage estimation. No clock, random id, system font or asset file is
  involved.
- Arms: `lavapipe` (the software Vulkan reference, ADR-009), `vulkan` (the host's own Vulkan
  device), `webgpu`, `metal` (reserved by ADR-053) and `null`. The null backend records commands
  but has no image output, so it only proves that the same scene emits the same command counts as
  the reference. An arm that is not built or has no device prints a `backend '<name>': skipped
  (<reason>)` line; it is never silently counted as a pass. ctest reports the reference run as
  skipped when lavapipe is unavailable, and the parity run still renders and checks determinism.
- The reference images are written by `gpu_backend_parity --write-reference` on the lavapipe
  device only (ADR-009: a real GPU never produces a reference). A per-pair comparison needs both
  the reference images and a candidate arm; without them the run says so and exits 0.
- Each pixel arm is rendered twice and must be byte-identical across the two runs; a difference
  fails the run as nondeterminism before any perceptual comparison happens.
- A failing pair writes the `make_diff_image` PNG (`parity/diff/<pair>_<scene>.diff.png`) and the
  JSON report (`parity/parity-report.json`) into the test output directory, and the harness prints
  both paths. The report names the pair, the verdict, the policy it used, SSIM / deltaE, the
  difference counts and the worst region (section 4).

## 2. Directory layout

```text
tests/golden/
├── baseline/
│   └── batched2d/
│       ├── dashboard.png
│       └── dashboard.report.json
├── cases/
│   └── batched2d/
│       ├── dashboard.scene.json
│       ├── dashboard.events.json
│       └── dashboard.case.json
└── out/
    └── <run-id>/
        ├── dashboard.png
        ├── dashboard.diff.png
        └── dashboard.report.json
```

- `baseline/` is committed.
- `cases/` contains the scene description and deterministic inputs.
- `out/` is generated and ignored.
- GPU-specific baselines/cases use the same `baseline/cases/out` structure under
  `tests/golden/gpu/`; see [`../design/gpu/testing.md`](../design/gpu/testing.md).

## 3. Case file

```json
{
  "caseVersion": 1,
  "name": "batched2d/dashboard",
  "backend": "software",
  "size": { "width": 512, "height": 512 },
  "scale": 1.0,
  "locale": "en-US",
  "fonts": ["tests/fixtures/fonts/Inter-Regular.ttf"],
  "scene": "dashboard.scene.json",
  "events": "dashboard.events.json",
  "compare": { "mode": "exact" },
  "tags": ["batched2d", "dashboard"]
}
```

| Field | Required | Notes |
|---|---|---|
| `caseVersion` | yes | `1` |
| `name` | yes | Unique, stable |
| `backend` | yes | `software` for goldens; `vulkan`/`webgpu` for smoke |
| `size` | yes | Logical/physical pixels |
| `scale` | yes | DPI scale (physical pixels per logical pixel); fixed per case, the example uses `1.0` |
| `locale` | no | Pinned locale |
| `fonts` | no | Committed font files |
| `scene` | yes | Scene/IR/draw-list input |
| `events` | no | Synthetic input/OS script |
| `compare.mode` | no | `exact` (software) or `perceptual` (GPU) |
| `compare.tolerance` | no | SSIM (structural similarity index) / ΔE (colour difference) thresholds for GPU smoke only |
| `tags` | no | CI filtering |

## 4. Comparison

### Software path

- Decode both PNGs to RGBA8.
- Dimensions must match.
- Byte-exact per pixel; alpha is compared too.
- A single differing pixel fails.

### GPU smoke

- Perceptual comparison only (SSIM/ΔE), with backend/driver-specific tolerance.
- Never used to bless a golden.
- Failure still emits a diff/report; tolerance is documented per case.

### Cross-backend perceptual comparison (issue #245)

The metric lives next to the per-channel delta in `rendering/golden/` (`perceptual.hpp`,
`difference_class.hpp`, `parity.hpp`); `compare_rgba` and its callers are unchanged. Two images
of the same scene are measured in three ways, and a pair policy (section 4.4) decides the
verdict.

#### SSIM on luma

SSIM (Wang et al. 2004) is computed on the Rec.709 luma of the 8-bit sRGB-encoded pixels:
`(54 R + 183 G + 19 B + 128) >> 8`. The coefficients are the standard ones quantized to 256ths and
their sum is exactly 256, so an achromatic pixel keeps its channel value (which keeps the
hand-computed unit tests simple). Windows are uniform 8x8 squares (the edge windows are clipped to
the image), moments are population moments, `L = 255`, `C1 = (0.01 L)^2`, `C2 = (0.03 L)^2`.
Identical images return exactly 1.0 for every window, which is what lets an exact pair require
`ssim >= 1.0`. The report carries the mean over windows and the worst window.

#### deltaE

deltaE is the CIE76 (plain Euclidean) distance in CIELAB. The documented colour pipeline is
sRGB (IEC 61966-2-1) encoded 8-bit -> linear light -> CIE XYZ under D65 (matrix and white point as
in CIE 15:2004, `Xn = 0.95047, Yn = 1, Zn = 1.08883`) -> CIELAB with the CIE companding constants
(`epsilon = 216/24389`, `kappa = 24389/27`). Alpha is ignored: parity compares painted colour, not
coverage storage. Identical images return exactly 0.0. The report carries the mean and the maximum
per-pixel deltaE; the pair policy gates the mean, the maximum is diagnostic.

#### Difference classes

Every differing pixel (any RGBA channel differs) is classified as one of:

- **edge rounding**: its largest per-channel delta is at most the pair's `tolerance` and it lies
  within the pair's edge radius (2 px) of a luma gradient (a step of at least 8 levels to a
  4-neighbour) in either image. This is antialiasing, SDF coverage or filtering rounding
  differently between backends, and only where the image already changes quickly and only a
  little.
- **structural**: everything else, including a delta larger than `tolerance` (a one-pixel shifted
  geometry edge flips fully, so it is structural even though it is on an edge), a difference in a
  flat region (wrong colour) and a region the other backend did not draw (missing draw).

The classifier reports the count of each class and the worst region (a 16x16 grid cell with the
largest structural count; when there are no structural pixels, the largest edge-rounding count;
ties go to the smallest y then x, so the report is deterministic). Structural differences are
never tolerated: a single one fails the pair. The classes are pinned by synthetic-image unit tests
(a shifted quad is structural; a one-level difference on the edge ring is edge rounding and passes
the listed pair, while it fails an exact pair).

#### Verdict order

A pair is checked in this fixed order, and the report names the first failing rule: image size
mismatch, structural pixel count (`> 0` fails), edge-rounding ratio (`<= max_mismatch`), mean SSIM
(`>= ssim`), mean deltaE (`<= delta_e`).

### Per-backend-pair tolerances (`tests/golden/tolerance.json` schema 2)

`tests/golden/tolerance.json` keeps its per-test entries (`tolerance`, `max_mismatch`) and, under
a test entry, gains an optional `pairs` object keyed by `<candidate>_vs_<reference>`:

```json
"gpu_backend_parity": {
  "tolerance": 0,
  "max_mismatch": 0.0,
  "pairs": {
    "vulkan_vs_lavapipe": {
      "tolerance": 8,
      "max_mismatch": 0.01,
      "ssim": 0.99,
      "delta_e": 2.0
    }
  }
}
```

`tolerance` is the per-channel allowance that may also be classified as edge rounding,
`max_mismatch` is the fraction of pixels that may be edge rounding, `ssim` the mean-SSIM floor and
`delta_e` the mean-deltaE ceiling. A pair that is not listed is **exact**: tolerance 0,
max_mismatch 0.0, ssim 1.0, delta_e 0.0. The `metal_vs_lavapipe` pair is listed as the policy the
Metal backend (ADR-053) will be held to. Schema 2 accepts schema 1 documents and entries unchanged
(they simply have no `pairs`); a schema 1 document that carries `pairs` is rejected. CMake reads
the file at configure time (through `scripts/golden_tolerances.py`), turns the pair table into
`parity_tolerances.hpp`, and a malformed file fails the configure step instead of silently
loosening a comparison.

## 5. `report.json`

```json
{
  "reportVersion": 1,
  "case": "batched2d/dashboard",
  "backend": "software",
  "size": { "width": 512, "height": 512 },
  "pass": false,
  "compare": { "mode": "exact", "different_pixels": 42 },
  "diff": {
    "image": "dashboard.diff.png",
    "bounds": { "x": 120, "y": 64, "width": 32, "height": 16 }
  },
  "nodes": [
    {
      "id": "node::42",
      "bounds": { "x": 120, "y": 64, "width": 32, "height": 16 },
      "dirty": ["paint"],
      "color": "#38bdf8"
    }
  ],
  "metrics": {
    "draw_calls": 12,
    "vertices": 480,
    "frame_time_ms": 0.0
  },
  "repro": {
    "command": "batched2d_poc --backend null --golden ...",
    "events": "dashboard.events.json"
  }
}
```

Reports include structural data even when images pass, so agents can reason about layout.
In `nodes[].dirty`, values are dirty domains (`style`, `layout`, `paint`, ...) as defined in
[`dirty-rendering.md`](../design/dirty-rendering.md).

### 5.1 Cross-backend parity report

The parity harness writes its own `parity-report.json` next to the images (`parity-report.json` in
the test output directory). It is a run document, not a per-case report:

```json
{
  "reportVersion": 1,
  "reference": "lavapipe",
  "compared": true,
  "verdict": "fail",
  "arms": [{ "name": "vulkan", "state": "compared", "detail": "" }],
  "scenes": [
    {
      "scene": "solid_quad",
      "pairs": [
        {
          "pair": "vulkan_vs_lavapipe",
          "status": "fail",
          "reason": "structural difference: 4 pixels (worst region 16,16 16x16)",
          "policy": { "edge_tolerance": 8, "max_mismatch": 0.01, "min_ssim": 0.99, "max_delta_e": 2.0 },
          "metrics": { "ssim": 0.999, "ssim_worst": 0.998, "delta_e_mean": 0.25, "delta_e_max": 1.0 },
          "counts": { "pixels": 12288, "mismatched": 4, "edge_rounding": 0, "structural": 4 },
          "worst": { "class": "structural", "x": 16, "y": 16, "width": 16, "height": 16, "pixels": 4 },
          "artifacts": {
            "actual": "parity/actual/vulkan/solid_quad.png",
            "reference": "parity/reference/solid_quad.png",
            "diff": "parity/diff/vulkan_vs_lavapipe_solid_quad.diff.png"
          }
        }
      ]
    }
  ],
  "structure": [
    { "arm": "null", "scene": "solid_quad", "draw_calls": 1, "vertices": 6, "matches_reference": true }
  ]
}
```

`arms[].state` is `reference`, `compared`, `structure-only` or `unavailable` (with the reason in
`detail`). `structure[]` is the null arm's command-count parity check. The document is stable text
(sorted scenes, pairs and arms in a fixed order, no timestamps or absolute paths beyond the
artifacts the run already printed).

## 6. Update policy

```bash
./tests/golden/update --case batched2d/dashboard   # target command (not implemented yet)
```

- Updates are explicit, reviewed, and separate from behavior changes.
- The report records the generator/backend version.
- A golden update PR must explain the visual change.
- Never auto-update on CI.

## 7. Determinism checklist

- [ ] fixed simulation clock/delta
- [ ] fixed offscreen size/scale/DPI
- [ ] committed fonts; no system fonts
- [ ] fixed locale/time zone/ICU data (ICU = International Components for Unicode)
- [ ] synthetic inputs/events
- [ ] no network/filesystem outside fixtures
- [ ] deterministic executor/ordering (executor: see [`async.md`](../design/async.md))
- [ ] no random ids/timestamps in scene/report

## 8. CI

- Linux: lavapipe/software path, `ctest` golden target.
- Windows: software path if one is available on the runner (this spec does not say which);
  otherwise Vulkan smoke only.
- Golden artifacts uploaded on failure (image, diff, report).
- Baseline changes require review; CI never writes baselines.
- Cross-backend parity (section 1.3): on the Linux lane the lavapipe ICD is selected process-wide,
  so the parity run's arm is the reference and no second device exists; a real-GPU lane (the
  self-hosted Windows machine) renders `vulkan` and compares it against the lavapipe reference
  images. `gpu_backend_parity_reference` is reported as skipped on a host without lavapipe, and
  the WebGPU arm names its missing host device. Pair failures still upload the diff PNG and the
  parity report.

## 9. AI-agent integration

The agent receives:

- pass/fail
- image diff
- `report.json` (node bounds/colors/dirty, metrics)
- reproduction command/event script

The agent should fix the structural cause, not patch pixels. Updating goldens is a human
decision unless the task explicitly allows it.

## 10. Versioning

- `caseVersion`/`reportVersion` bump on schema changes.
- Adding optional fields keeps the version; changing comparison semantics bumps it.
- Baselines record the renderer/compiler version used to produce them.
- `tests/golden/tolerance.json` schema 2 (issue #245) adds the optional per-test `pairs` object.
  Schema 1 documents and entries stay valid unchanged; a schema 1 document with `pairs` is
  rejected, because the schema bump is what makes the stricter pair semantics explicit. The
  cross-backend parity report keeps `reportVersion` 1 for the same reason: it is a new document
  type, not a change to the per-case `report.json`.
