# Golden tests v1

Status: proposed (Big Step B2/B3). Normative for visual regression; ADR-009 is the decision.

## 1. Rules

1. Golden images come from a deterministic software path, never a real GPU framebuffer.
2. Bit-exact comparison on the software path; any diff fails unless explicitly updated.
3. GPU runs are smoke tests with perceptual tolerance; they do not produce/update baselines.
4. Fixed clock, size, DPI, fonts, locale, inputs, and event scripts.
5. Golden updates are a separate, reviewed change.
6. Failures emit an image diff plus structural `report.json` for humans/agents.

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
| `scale` | yes | DPI scale; goldens usually `1.0` |
| `locale` | no | Pinned locale |
| `fonts` | no | Committed font files |
| `scene` | yes | Scene/IR/draw-list input |
| `events` | no | Synthetic input/OS script |
| `compare.mode` | no | `exact` (software) or `perceptual` (GPU) |
| `compare.tolerance` | no | SSIM/ΔE thresholds for GPU smoke only |
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
- [ ] fixed locale/time zone/ICU data
- [ ] synthetic inputs/events
- [ ] no network/filesystem outside fixtures
- [ ] deterministic executor/ordering
- [ ] no random ids/timestamps in scene/report

## 8. CI

- Linux: lavapipe/software path, `ctest` golden target.
- Windows: software path if available; otherwise Vulkan smoke only.
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
