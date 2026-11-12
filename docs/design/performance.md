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

### 3.1 Memory budgets

Frame budgets say nothing about resident memory, which regresses late and on mobile first (issue
#13). Measured numbers, enforced where noted:

| Item | Cost | Enforced by |
|---|---|---|
| `ui::Node` (plain View) | about 290 bytes, budget `kNodeSizeBudget` = 320 | `static_assert` in `node.hpp`, `node_size` test |
| ARIA table (first `aria-*`) | about 520 bytes, once per node | lazy; `node_size` test counts allocations |
| Inline-style table (first declaration) | about 520 bytes, once per node | lazy; `node_size` test |
| Event-binding table (first binding) | about 200 bytes, once per node | lazy; `node_size` test |
| Paint payload (text/image node) | about 580 bytes plus its string | lazy; `node_size` test |
| `InputEvent` | 344 bytes (the 256-event queue is about 86 KiB) | measured, not yet asserted |
| Resident bytes per plain node (20k-node tree) | about 470 bytes measured, budget 1024 | `memory_steady_state` test, `tests/golden/memory/steady_state.txt` |
| Idle cycles after warm-up | 0 bytes of RSS growth measured, budget 2 MiB | `memory_steady_state` test |
| Headless generated-UI sample: build and layout | about 0.6 MB measured, budget 8 MiB | `generated_ui_memory` test, `tests/golden/memory/generated_ui.txt` |
| Same sample: first paint | about 0.9 MB measured, budget 8 MiB | `generated_ui_memory` |
| Same sample: 300 click + repaint cycles after warm-up | 0 to 45 KB measured, budget 1 MiB | `generated_ui_memory` |
| Same sample: process peak resident | about 8.7 MB measured, budget 64 MiB | `generated_ui_memory` |

A node carried 1984 bytes before the rarely used tables became lazily allocated, so a 10k-node
screen of plain views drops from about 20 MB to about 3 MB of node storage (Yoga node and style
storage are extra). Clearing a table keeps its allocation, so toggling a value never reallocates.
`diagnostics::sample_process_memory()` (`diagnostics/memory.hpp`) reads resident and peak bytes
with no heap allocation (VmRSS/VmHWM on Linux and Android, the working set on Windows,
`resident_size` on macOS and iOS); `MemoryGauges` publishes them as the gauges `memory.rss_bytes`
and `memory.rss_peak_bytes`. It is a syscall: sample per second or per scripted checkpoint, never
per frame. Real RSS is machine dependent, so the committed bounds are generous and catch leaks and
order-of-magnitude regressions, not noise. Raise a budget only with a recorded decision. Still to
measure and budget: Android PSS (needs the Android sample). The `generated_ui_memory` test runs the
compiler-generated demo headless (CPU canvas, no GPU), samples resident memory after the build, the
first paint and a scripted click loop, and prints the numbers on every run so CI logs show the
trend; it fails above the committed bounds.

GPU memory: `rhi::Device::memory_statistics()` returns the allocator totals (`block_bytes` asked of
the driver, `allocation_bytes` handed to live buffers and images, and their counts). The Vulkan
backend fills it from `vmaCalculateStatistics`; other backends report zero. It is a snapshot with no
GPU work, for checkpoints rather than every frame. `buffer_bytes` (exact) and `image_bytes`
(estimated as extent x texel size, x samples plus the resolve image for a multisampled target) split
the live resources by kind. Swapchain images are driver-owned and not counted. Measured costs, which
`rhi_memory` asserts: a 1024x1024 R8 glyph atlas page is 1 MiB; a 1280x720 RGBA target is 3.5 MiB,
and 17.6 MiB at 4x MSAA. A texture upload also holds a staging buffer of the same size until the
copy finishes. `diagnostics::GpuMemoryGauges` publishes the four numbers as `gpu.buffer_bytes`,
`gpu.image_bytes`, `gpu.block_bytes` and `gpu.allocation_bytes`.

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
- **Occluded/minimized**: `pauseWhenOccluded=true` stops rendering and animation ticks; the loop
  keeps pumping events/input and parks on the bounded event wait, and the resume edge repaints the
  full target (implemented in `WindowHost`,
  [`window-resize.md`](window-resize.md#8-occludedminimized-pause-b2-power-slice)).
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

### 6.1 Implemented decision layer (step 2 reports only)

The decision layer is implemented. **It does not scale pixels yet**: `renderScale` still has no
render-target consumer, so a downgrade changes a reported number, never the resolution
([`runtime-config-v1.md`](../specs/runtime-config-v1.md) section 6.2). Step 1 (effects) and step 3
(present cap) are not implemented either; step 5 (report) is.

`src/runtime/adaptive_quality.hpp` is a pure state machine fed one frame observation at a time
(frame time in ms plus the `adaptiveQuality`, `powerPolicy`, `renderScale`, `minRenderScale` and
`frameBudgetMs` fields). It is deterministic — no clock, no RNG, no locale, no allocation, not even
at construction (the type is trivially copyable) — so an observation sequence always yields the same
decision sequence.

| Constant | Value | Why |
|---|---|---|
| Downgrade run | 4 consecutive missed budgets | one slow frame is noise; four at 60 Hz is ~66 ms of sustained overrun |
| Upgrade run | 12 consecutive met budgets | recovery is deliberately slower than the downgrade, so a borderline machine does not ping-pong |
| Cooldown | 8 observed frames after a change | no two changes within it: at most one step per ~133 ms at 60 Hz |
| Ladder | 4 steps from `renderScale` (step 0) to `minRenderScale` (step 4) | bounded, evenly spaced steps; no fractional creep |

Each change moves exactly one ladder step, clamped to `[minRenderScale, renderScale]`;
`minRenderScale == renderScale` leaves no room to move, so it never changes. `adaptiveQuality=false`
and `powerPolicy=benchmark` disable the controller completely. Cooldown frames are not evidence for
the next run, so a sustained overload waits out the cooldown and then a full downgrade run, and an
alternating miss/met sequence changes nothing.

The frame path feeds one observation per presented frame: `AppHost::observe_frame_time_ms`, called
by `WindowHost` with the frame's CPU cost through present (GPU timestamps are section 7's
follow-up). A change is recorded in `runtime::FrameDiagnostics` (`adaptive_quality()`,
`adaptive_quality_changes()`, `adaptive_quality_downgrades()`), so a downgrade is visible without
sampling frame time, and it survives `AppHost::resize()`, which only re-creates the pipeline.

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
