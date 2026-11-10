# Performance, power, and hardware utilization

Status: design for Big Step B2 (runtime slice) and B3 (window/swapchain path); both are defined in
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step). Companion to [`game-loop.md`](game-loop.md),
[`dirty-rendering.md`](dirty-rendering.md), and [`startup-and-loading.md`](startup-and-loading.md).
Normative decision:
[ADR-019](../../decisions/ADR-019-power-aware-runtime.md) (power-aware runtime defaults). Config keys
named here (`maxFps`, `powerPolicy`, `fullRedrawThreshold`, ...) are defined in
[`runtime-config-v1.md`](../specs/runtime-config-v1.md). Terms (tick, damage, VRR, Yoga, dirty domain) are
in [`glossary.md`](../reference/glossary.md); tick = one fixed-step update ([`game-loop.md`](game-loop.md)).

## 1. Position: application framework, not a benchmark

Tessera is a framework for building applications, not a game or a GPU benchmark. The default
must be **power-aware**:

- a static screen performs **no GPU work** and presents **no frames** after it settles;
- work is proportional to what changed, not to tree size or a fixed frame rate;
- the loop never busy-waits, never spins on the GPU, and never tries to reach 100% utilization;
- a fixed timestep exists for deterministic simulation, **not** to force continuous rendering.

A game engine commonly runs an uncapped, always-rendering loop to maximize frame rate. That is a
valid choice for a game and a bad default for a desktop/industrial application. In Tessera,
continuous rendering is an explicit mode for video/live dashboards, not the baseline.

### 1.1 Game engine vs application runtime

| Aspect | Game engine (as commonly built) | Tessera application runtime |
|---|---|---|
| Loop | continuous, often uncapped | fixed tick + on-demand render |
| Static screen | still renders every frame | no presents, no GPU submissions |
| GPU goal | maximize utilization/frame rate | work proportional to change; idle |
| Frame rate | as high as possible | cap/vsync/VRR; idle between changes |
| Power | performance first | power-aware/adaptive by default |
| Full redraw | normal | dirty/damage; conservative fallback |
| Benchmark mode | normal | explicit diagnostic only |

"Fixed timestep" is a determinism mechanism, not a load mechanism. It does not imply that the
GPU is driven continuously.

## 2. What determines hardware utilization

GPU/CPU usage is the product of these choices, not a single switch:

| Factor | Effect | Control |
|---|---|---|
| Present rate | Frames submitted per second | `maxFps`, `vsync`, VRR, `renderOnDemand` |
| Dirty work | Nodes/styles/layouts repainted | dirty domains, `dirtyRendering` |
| Damage area | Pixels redrawn | `damageRects`, `fullRedrawThreshold` |
| Resolution | Shaded pixels | `renderScale`, adaptive quality |
| Overdraw | Repeated fragment work | batching, opaque-first ordering, layer isolation |
| Effects | Blur/shadow/alpha/AA cost | style/property choices, adaptive quality |
| Draw calls/binds | CPU/driver overhead | batching key `(clip, texture)`, descriptor cache |
| Texture uploads | PCIe/memory bandwidth | atlas dirty regions, upload queue |
| Frames in flight | CPU/GPU overlap and latency | `gpuFramesInFlight`, `latencyFrames` |
| Animations | Tick and repaint frequency | active-animation tracking, pause when hidden |
| Visibility | Occluded/minimized work | `pauseWhenOccluded`, `backgroundMaxFps` |
| Text/layout | Shaping and Yoga cost | cached shaped runs, dirty subtrees |

There is no portable, reliable "GPU utilization %" signal. Drivers/OS expose different,
sometimes coarse, metrics. The runtime controls **frame time, work counters, and present
behavior**, not a utilization percentage. Platform GPU-load queries are diagnostics only.

### 2.1 What does *not* determine utilization

- `fixedTimestepHz` — simulation only; a 60 Hz tick with a static screen renders nothing.
- `maxFps=0` — "no cap", not "render as much as possible". On-demand still idles.
- `powerPolicy=performance` — explicit continuous mode, not the default.
- A GPU utilization percentage — not a control signal; frame budgets are.
- A dirty flag alone — work still stops at the next clean frame; damage/caches bound the cost.

## 3. Budgets

- **Frame budget**: target CPU+GPU time per presented frame. Derived from the effective rate
  (`1000 / effective_fps`) unless `frameBudgetMs` is set.
- **CPU budget**: input + style + layout + paint + command recording.
- **GPU budget**: measured with timestamp queries when enabled; otherwise inferred from frame
  time and present wait.
- **Input latency budget**: input sample -> visible frame; bounded by `latencyFrames`.
- **Headroom**: a UI framework should leave headroom for the OS/compositor and other apps. A
  frame that exactly uses 100% of the budget has no safety margin.

Metrics report `frame_time_ms`, `update_time_ms`, `style_time_ms`, `layout_time_ms`,
`paint_time_ms`, `render_time_ms`, `present_wait_ms`, `dropped_steps`, and `missed_deadlines`.
Adaptive quality uses these, not a utilization target. Measurement/tracing is defined in
[`diagnostics.md`](diagnostics.md).

## 4. Power policies

`runtime.powerPolicy` selects a coherent set of defaults:

| Policy | Present | Dirty | Adaptive | Use |
|---|---|---|---|---|
| `adaptive` (default) | on-demand + cap | on | on | normal apps |
| `power-save` | on-demand, lower cap | on | aggressive | battery/kiosk |
| `balanced` | on-demand + cap | on | moderate | desktop apps |
| `performance` | continuous/cap | on | off | live dashboards, video |
| `benchmark` | uncapped | off (full redraw) | off | profiling only; never shipped |

Rules:

- `adaptive` is the default. It starts on-demand, renders on wake/dirty, and lowers quality only
  when deadlines are missed.
- `performance` may render continuously but still honors `maxFps` and dirty work.
- `benchmark` is an explicit diagnostic mode. It is not a production default and must be
  reported in logs/metrics.
- A policy may adjust `maxFps`, but it must not silently change simulation correctness.

## 5. Idle and background behavior

- **Static scene**: after the last dirty frame, no present and no GPU submission. CPU waits on
  the next wake source.
- **Input**: a pointer/key event wakes exactly one frame (or, during a drag or active animation, consecutive frames until it ends; the burst length is not specified).
- **Occluded/minimized**: `pauseWhenOccluded=true` stops rendering and active animations.
  Restoring the window marks the target dirty and renders one frame.
- **Unfocused but visible**: optional `backgroundMaxFps` (default `0`); `0` means use `maxFps`.
- **VRR**: in on-demand mode, the display idles between frames; do not present empty frames to
  "keep VRR alive".
- **Timers/data**: wake sources are edge-triggered. Polling the filesystem/network from the loop
  is forbidden.

## 6. Adaptive quality

Signals:

- missed deadlines / frame time above budget
- damage ratio and full-redraw frequency
- GPU timestamp duration (when enabled)
- input-to-present latency
- queue depth / dropped steps

Actions, in order:

1. reduce optional effects (blur, large shadows, extra AA)
2. lower `renderScale` toward `minRenderScale`
3. lower the present cap to the next refresh divisor
4. pause non-essential animations
5. report a quality downgrade in metrics

Step 2 moves the adaptive `renderScale`; the monitor-derived device scale is a separate value that
quantizes onto a fixed ladder and keys text/glyph rasterization ([`window-resize.md`](window-resize.md)
§2.3). Adaptive quality never changes the device scale, so a quality adjustment does not
re-rasterize glyphs or resize the text atlas.

Never:

- skip correctness/dirty invalidation
- lower input responsiveness
- silently change simulation dt
- oscillate every frame (use hysteresis and a cooldown)

All adaptive changes must be reversible and visible in diagnostics.

## 7. GPU utilization and platform metrics

- Vulkan: timestamp queries, `VK_EXT_calibrated_timestamps`, present wait.
- Windows: DXGI frame statistics, waitable swapchain, ETW (Event Tracing for Windows)/GPU scheduler counters, if enabled.
- Linux: DRM `fdinfo`, GPU-specific counters (NVML/ADL) only for diagnostics; not portable.
- Never gate correctness or frame pacing on a vendor-specific utilization number.

## 8. Anti-patterns

- Uncapped busy-wait loop or `while(true) render()`.
- Rendering a static UI at 60/144 Hz because "the engine is a game loop".
- Full redraw for a single sparkline update.
- Full-tree style/layout rebuild on every input event.
- Per-frame allocations, pipeline/descriptor churn, or readbacks.
- Keeping animations running while occluded/minimized.
- Targeting "100% GPU" as a success metric.
- Polling devices/filesystems from the frame loop.

## 9. Acceptance and tests

- Static scene: after settling, **0 presents and 0 GPU submissions** for N seconds (N is not specified).
- Input wake: exactly the expected frame(s), then idle again.
- 60 fps animation: within budget, with headroom; no full-load behavior.
- Occluded/minimized: no rendering, no active animation ticks.
- Power policy: `adaptive`/`power-save` reduce work; `benchmark` is explicit.
- Adaptive quality: downgrade then recover with hysteresis; visual correctness preserved.
- Long-run: no unbounded cache growth, no leaked frames, no growing queues.

## 10. Open questions

- Per-platform or battery-state overrides of `backgroundMaxFps` (the default is `0` on all platforms per [`runtime-config-v1.md`](../specs/runtime-config-v1.md); no per-platform override is decided).
- Thermal/power-source signals and whether to use them at all (no decision yet; HAL `PowerState` in [`hal.md`](hal.md) exposes them).
- ~~Per-monitor render scale and mixed-DPI multi-window.~~ Decided: one render scale per window in
  V1, derived from that window's `WindowMetrics` and recomputed only on a DPI or monitor change
  ([`window-resize.md`](window-resize.md) §2.3). Per-monitor scale is deferred until multi-window
  or per-display rendering is scheduled or a mixed-DPI app needs it.
- How much adaptive quality is acceptable without visual regression.
- Benchmark harness and baseline budgets per scene class.
