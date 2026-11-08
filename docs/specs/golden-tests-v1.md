# Golden tests v1

Status: partially implemented (Big Steps B2 and B3, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big
Steps"). The batched2d harness compares a lavapipe baseline through
`rendering/golden/image_diff.hpp` and `batched2d_poc --compare`; `--report`/`--diff` artifacts
exist, while the CPU-rasterizer path and wider corpus are still proposed. Normative for visual regression;
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
antialiasing rounds differently across lavapipe/LLVM revisions. `--report` and `--diff` write
structural/visual artifacts; the
CPU-rasterizer path and larger corpus are not implemented yet.

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
