# ADR-009: Deterministic headless golden tests on a software rasterization path

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-001, ADR-002, ADR-003, ADR-006

## Context

Visual regression testing is a core feature: it lets both humans and AI agents detect layout
and rendering regressions. But GPU rendering is not bit-identical across drivers, GPUs, or
even driver versions. Comparing raw GPU pixels produces flaky tests.

Tessera already needs an offscreen path for the AI-agent loop. The question is which path is
allowed to produce the golden images.

## Decision

1. **Golden images come from a deterministic software path**, never from a real GPU
   framebuffer:
   - Vulkan parity: **Mesa lavapipe** (software Vulkan ICD)
   - software backend: our CPU rasterizer, kept deliberately simple
2. **Fixed simulation clock**: golden runs use a fixed delta (e.g. 16.666 ms) and never read
   the wall clock during layout, animation, or rendering.
3. **Fixed inputs**: committed font files, fixed DPI/scale, fixed window/offscreen size,
   deterministic text shaping, no subpixel positioning in golden mode.
4. **GPU runs are smoke tests only.** They may use perceptual tolerance (SSIM / ΔE) but do not
   produce or update goldens.
5. **Bit-exact comparison** for the software path. Any diff is a failure unless a reviewed
   `--update-goldens` run explicitly rewrites the baseline.
6. Baselines live in `tests/golden/baseline/`; diffs and a machine-readable report
   (`report.json`: node ids, bounds, colors, diff regions) are emitted next to them.
7. The report is the AI-agent feedback channel: structural layout errors plus the image diff,
   not just a pass/fail bit.
8. Golden updates must be a separate, reviewed change.

## Consequences

### Positive

- Reproducible tests across machines and CI.
- The AI-agent loop gets stable visual + structural feedback.
- CI does not need a GPU; lavapipe runs on a normal runner.

### Negative / risks

- Software rendering is slower than GPU rendering.
- CI must install and maintain lavapipe.
- Software and GPU output can diverge; GPU smoke tolerance must be tuned.
- Our CPU rasterizer is another component to keep correct.

## Alternatives considered

- **GPU pixels as goldens.** Rejected: flaky across hardware and drivers.
- **SwiftShader instead of lavapipe.** Possible alternative; lavapipe is chosen because it is
  the Mesa reference software Vulkan implementation and easy to pin on Linux CI. Revisit if
  SwiftShader gives better Vulkan 1.3 coverage.
- **Layout/DOM assertions only.** Rejected: misses visual regressions (colors, borders,
  antialiasing, clipping).
- **Perceptual diff on GPU only.** Rejected: still hardware-dependent and hides real diffs.

## Follow-up

- Add the golden runner and baseline layout in Big Step E.
- Lock the lavapipe version in CI.
- Define the `report.json` schema together with the AI-agent integration.
