# Tessera documentation map

Start with [`../AGENTS.md`](../AGENTS.md) for the operating rules and [`../README.md`](../README.md)
for the project overview. This page is the reading order for the runtime work.

## 1. Decisions

[`../decisions/README.md`](../decisions/README.md) is the index of ADRs. The runtime-related
ones are:

- [ADR-005](../decisions/ADR-005-yoga-layout-adapter.md) — Yoga Flexbox + small Grid subset
- [ADR-009](../decisions/ADR-009-deterministic-headless-golden-tests.md) — deterministic goldens
- [ADR-016](../decisions/ADR-016-stylesheet-engine.md) — stylesheet engine
- [ADR-017](../decisions/ADR-017-input-and-fixed-step-runtime.md) — input + fixed-step loop
- [ADR-018](../decisions/ADR-018-dirty-rendering-and-frame-pacing.md) — dirty rendering +
  frame pacing/VRR
- [ADR-019](../decisions/ADR-019-power-aware-runtime.md) — power-aware defaults; no full-load loop

## 2. Specs (normative)

- [`specs/project-config-v1.md`](specs/project-config-v1.md) — `tessera.json` project schema
- [`specs/runtime-config-v1.md`](specs/runtime-config-v1.md) — FPS cap, vsync/VRR, pacing,
  dirty rendering, deterministic mode
- [`specs/stylesheet-v1.md`](specs/stylesheet-v1.md) — full CSS syntax + semantic matrix
- [`specs/dependencies.md`](specs/dependencies.md) — canonical dependency and license map

## 3. Design

- [`design/renderer.md`](design/renderer.md) — RHI, batched 2D, Vulkan backend
- [`design/scene-graph.md`](design/scene-graph.md) — node tree, hit-testing, events, dirty bits
- [`design/input.md`](design/input.md) — input normalization, focus, capture, IME
- [`design/game-loop.md`](design/game-loop.md) — fixed timestep, frame cap, vsync, VRR
- [`design/dirty-rendering.md`](design/dirty-rendering.md) — invalidation, caches, damage,
  multi-frame execution
- [`design/performance.md`](design/performance.md) — power-aware defaults, budgets, adaptive
  quality, hardware utilization

## 4. Guides

- [`guides/project-generator.md`](guides/project-generator.md) — create/configure/build a project
- [`guides/code-style.md`](guides/code-style.md) — style, file size, build-cache rules

## 5. Runtime reading order

For the stylesheet + input + loop slice:

1. [Stylesheet spec](specs/stylesheet-v1.md) — what a rule means and how it is computed.
2. [Scene graph](design/scene-graph.md) — what a style is applied to and how events target it.
3. [Input handler](design/input.md) — how pseudo-states and pointer/keyboard events arrive.
4. [Game loop](design/game-loop.md) — when simulation and render happen.
5. [Dirty rendering](design/dirty-rendering.md) — what work is actually done.
6. [Performance & power](design/performance.md) — why static UI idles and how utilization is
   bounded.
7. [Runtime config](specs/runtime-config-v1.md) — how the app tunes FPS/vsync/VRR/dirty/power.

## 6. Status

| Area | State |
|---|---|
| RHI + Vulkan batched 2D | B1 implemented |
| Scene graph + events | B1 implemented |
| Stylesheet engine | B2 proposed (ADR-016) |
| Input handler | B2 proposed (ADR-017) |
| Fixed-step loop | B2 proposed (ADR-017) |
| Dirty rendering + frame pacing | B2/B3 proposed (ADR-018) |
| Power-aware idle/adaptive defaults | B2 proposed (ADR-019) |
| Yoga layout / text / glyph atlas | B2 planned |
| Pixel goldens on lavapipe/software | B2/B3 planned |
