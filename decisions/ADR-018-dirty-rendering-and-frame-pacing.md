# ADR-018: Dirty rendering, frame pacing, and display refresh

- Status: Proposed
- Date: 2026-11-07
- Related: [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin render hardware interface and batched 2D renderer), [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests), [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (input and fixed-step runtime loop)

## Context

Tessera targets data-heavy dashboards and animated UI. Rendering the full tree every frame wastes
CPU/GPU on static screens; rendering on every display refresh wastes power when nothing changes.
At the same time, FPS caps, vsync modes, and variable refresh rate (VRR) interact in ways that
are easy to get wrong. Deterministic [goldens](../docs/reference/glossary.md) must not depend on display timing.

## Decision

1. **[Dirty domains](../docs/reference/glossary.md)**: `StyleDirty`, `LayoutDirty`, `PaintDirty`, `RenderDirty`, `ResourceDirty`,
   `GpuDirty`. Nodes store bits; ancestors aggregate `has_dirty_descendant`.
2. **Pipeline order**: style -> layout -> paint -> render. Dirty style raises layout/paint only
   when the corresponding computed groups changed; layout raises paint/render; paint raises
   render.
3. **[Retained/immediate hybrid](../docs/reference/glossary.md)**: stable subtrees cache draw lists/layers; dynamic charts stream
   into per-frame arenas. Caches are bounded and evictable.
4. **Damage rectangles** (screen regions that changed and must be repainted): accumulate old/new bounds, merge, cap at `maxDamageRects`, and fall
   back to full redraw when the damage ratio exceeds `fullRedrawThreshold` or when translucent
   overlap/backdrop effects make partial redraw unsafe.
5. **Simulation rate and present rate are independent**. The simulation is fixed-step ([ADR-017](ADR-017-input-and-fixed-step-runtime.md));
   presentation is controlled by `maxFps`, `vsync`, `dynamicRefreshRate`, and `framePacing`.
6. **FPS cap**: `maxFps=0` means uncapped; a non-zero cap is a hard present-rate cap. A cap below
   the display refresh uses a refresh divisor when possible; otherwise deadline pacing. Never
   busy-wait.
7. **Vsync/present modes**: `off` (immediate/tearing), `on` (FIFO), `adaptive`
   (FIFO_RELAXED), `mailbox` (replace queued frame). Fall back safely when unsupported.
8. **Dynamic refresh/VRR**: `dynamicRefreshRate=auto` uses VRR when the platform/display exposes
   it, presenting when the frame is ready; `off` uses the fixed/preferred rate; `preferred`
   requests `preferredRefreshRate`. VRR never overrides `maxFps`.
9. **[Render-on-demand](../docs/reference/glossary.md)**: static content renders only on wake sources (input, dirty, animation,
   data, resize, explicit request). Continuous mode remains available for video/live charts.
10. **Multi-frame execution**: `gpuFramesInFlight` 1–3, per-frame arenas, timeline semaphores;
    no readback or `deviceWaitIdle` in the frame loop.
11. **Deterministic/offscreen mode** disables pacing, vsync, damage, and interpolation-dependent
    timing; it always full-redraws into a fixed target.
12. **Metrics**: frame/update/paint/render timings, steps, dropped steps, effective FPS, refresh,
    VRR state, present mode, dirty counts, draw calls, damage ratio, cache hit rate.
13. **Platform fallbacks**: missing refresh/VRR/damage capability assumes 60 Hz, `vsync=on`, no
    VRR, full redraw, and records the assumption.

## Consequences

### Positive

- Static screens cost almost nothing; dynamic screens pay for what changed.
- Stable, explicit FPS/VRR behavior instead of accidental present pacing.
- Deterministic goldens remain independent of display timing.
- Metrics make regressions visible.

### Negative / risks

- Dirty/damage correctness is subtle; a missed invalidation shows a stale frame. The fallback is
  conservative full redraw.
- VRR/damage platform behavior varies by driver/compositor.
- Layer/draw-list caches need budgets and eviction; an unbounded cache is a leak.
- [Frame pacing](../docs/reference/glossary.md) adds scheduling complexity and occasional jitter when caps are not divisors.

## Alternatives considered

- **Full redraw every frame.** Rejected: wastes CPU/GPU and power; unusable for large dashboards.
- **Mark the root dirty on every change.** Rejected: defeats the purpose and scales with tree size.
- **Uncapped busy-wait loop.** Rejected: burns CPU/power and fights VRR.
- **Fixed 60 Hz present only.** Rejected: wastes high-refresh displays and cannot follow VRR.
- **GPU readback to detect changes.** Rejected: stalls the pipeline; CPU-side invalidation is the
  source of truth.
- **Browser compositor scheduling.** Rejected: no browser in production.

## Follow-up

- Implement dirty flags and equivalence tests (dirty vs full redraw pixel-identical).
- Implement the offscreen full-redraw loop first; add damage/partial redraw with the window path.
- Implement window/swapchain pacing on Windows/Linux, then VRR/refresh queries.
- Add layer/draw-list cache budgets and eviction.
- Add performance stress tests and a debug overlay.
