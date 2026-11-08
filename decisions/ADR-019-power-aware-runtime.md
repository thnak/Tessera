# ADR-019: Power-aware runtime defaults; no full-load game loop

- Status: Proposed
- Date: 2026-11-07
- Related: [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests), [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (input and fixed-step runtime loop), [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering and frame pacing)

## Context

Tessera uses a fixed-timestep, game-engine-style core for deterministic simulation. That can be
misread as "run the GPU at full load like a game". For an application framework (dashboards,
tools, kiosks, industrial UI), maximizing GPU utilization is a bad default: it wastes power,
creates heat/noise, shortens battery life, and competes with other applications.

Hardware utilization is not a single switch. It is the result of present rate, dirty work,
damage area (screen regions that changed and must be repainted; see decision 4 of [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md)), render scale, effects/overdraw, frames in flight, and visibility. "GPU utilization
%" is not a portable or reliable control signal; frame time and work counters are.

## Decision

1. **Default to power-aware**: `powerPolicy=adaptive`, `renderOnDemand=true`,
   `dirtyRendering=auto`, capped/vsync-paced presenting, adaptive quality enabled.
2. **Fixed timestep is not continuous GPU work.** The simulation can be idle; rendering happens
   on dirty/wake events. Active animations/data are wake sources and render on schedule.
3. **No busy-wait and no full-load target.** The loop waits on timers/OS events/swapchain
   feedback. `powerPolicy=benchmark` (uncapped/full redraw) is diagnostic only and must not ship.
4. **Static scenes idle**: after settling, zero presents, zero GPU submissions, zero animation
   ticks. Input/data/resize/theme wakes the minimum frames needed.
5. **Occluded/minimized pauses** by default (`pauseWhenOccluded=true`). Unfocused-but-visible may
   be capped by `backgroundMaxFps`; `0` means use `maxFps`.
6. **Utilization is controlled by work**: [dirty domains](../docs/reference/glossary.md), damage rectangles, render scale,
   effects, batching, texture uploads, frames in flight, and visibility. There is no
   "maxGpuUtilization" knob.
7. **Budgets, not utilization percentages**: use frame/CPU/GPU time, missed deadlines, dropped
   steps, input latency, draw calls, damage ratio, and upload bytes. Platform GPU-load counters
   are diagnostics only.
8. **Adaptive quality** reacts to missed budgets with hysteresis: optional effects, then
   `renderScale` down to `minRenderScale`, then present-rate divisor, then pause non-essential
   animations. It never skips correctness, dirty invalidation, or input responsiveness.
9. **Explicit performance modes**: `performance` (continuous/live dashboards) and `benchmark`
   (profiling) are opt-in, visible in metrics, and never defaults.
10. **Acceptance**: a static scene must produce 0 presents/submissions; input wakes the expected
    frames and idles again; adaptive quality must recover without visual regressions.

## Consequences

### Positive

- The framework behaves like a good desktop/industrial app, not a benchmark.
- Laptops/battery devices, fanless panels, and kiosks are viable.
- Idle CPU/GPU cost is near zero for static screens.
- Performance work is measured by frame time and work counters, which are portable.

### Negative / risks

- On-demand behavior adds wake-tracking complexity and missed-wake bugs.
- Adaptive quality needs tuning/hysteresis; poor tuning can oscillate.
- VRR (variable refresh rate)/occlusion/platform feedback varies by OS/driver.
- Users coming from game engines may expect a game-engine "always 60/144 fps" behavior; it must be opt-in.

## Alternatives considered

- **Game-engine full-load loop.** Rejected: wrong for an application framework, wasteful.
- **Always render at 60/144 Hz.** Rejected: static UI would burn power for identical pixels.
- **Target a GPU utilization percentage.** Rejected: not portable/reliable; encourages the wrong
  behavior and ignores frame budgets.
- **On-demand only, no continuous mode.** Rejected: video/live dashboards and benchmarking need
  continuous presentation.
- **Pause everything when unfocused.** Rejected as a universal default: dashboards that must keep
  updating while unfocused exist; it is configurable.

## Follow-up

- Implement the adaptive policy, idle wake tracking, and occluded/minimized handling.
- Add acceptance tests for zero-work idle and exact input wake frames.
- Add benchmark scenes and per-scene frame budgets.
- Tune adaptive-quality thresholds/hysteresis on Windows and Linux.
- Keep [`docs/design/performance.md`](../docs/design/performance.md), [`docs/design/game-loop.md`](../docs/design/game-loop.md), and [`docs/specs/runtime-config-v1.md`](../docs/specs/runtime-config-v1.md) in sync.
