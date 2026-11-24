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
| `ui::Node` (plain View) | 320 bytes (Release; larger in MSVC Debug, where the layout asserts are skipped), budget `kNodeSizeBudget` = 320 (336 after the `::part()` metadata pointer, issue #235; 320 with the flags word of #285; 312 after the member regrouping of the security review, finding C4; 328 with the cached paint box of #735; 320 again after the `NodeScroll`/`NodeMetadata` split of #817) | `static_assert` in `node.hpp`, `node_size` test, `sizeof_node` row |
| ARIA table (first `aria-*`) | 520 bytes, once per node | lazy; `node_size` test counts allocations; `aria_table_bytes` row |
| Inline-style table (first declaration) | 520 bytes, once per node | lazy; `node_size` test; `inline_style_table_bytes` row |
| Event-binding table (first binding) | 200 bytes, once per node | lazy; `node_size` test; `event_binding_table_bytes` row |
| Paint payload (text/image node) | 520 bytes plus its string; 528 allocated on first use. The paint cache's recorded op is 104 bytes, down from 2376 while every draw payload sat inline (issue #731 made the payload a `std::variant` and moved the gradient shapes to `PaintRecord::gradients`, so the 2 MiB budget holds 1159 entries instead of 104) | lazy; `node_size` test; `sizeof_paint_data`, `paint_slot_bytes` and `sizeof_paint_op` rows; `paint_op_size` test |
| Resolved `background-image` | `BackgroundLayerList` 1672 bytes, down from 4552 because `GradientValue` carried all three gradient shapes side by side (24 stops where a gradient can use 8); issue #731 made the three shapes a union, so `GradientValue` is 200. `resolve_layers` returns one by value per painted node | `background_layer_size` test (`static_assert`), `sizeof_background_layer_list` row |
| `InputEvent` | 320 bytes (the 256-event queue is about 80 KiB; the members were grouped by alignment in the security review, finding C5, which removed 24 bytes of internal padding) | `sizeof_input_event` row (was measured but unasserted) |
| `ComputedStyle` (first property) | 128 bytes per styled node (208 in MSVC Debug); the handle itself is 16 bytes on MSVC, 48 on libstdc++ | `computed_style_one_property_bytes` row |
| `NodeId` | 8 bytes, and it is repeated in every side table (paint cache, resolved styles, dirty list, damage) | `sizeof_node_id` row |
| Internal padding (issue #731) | `PaceDecision` 24→16, `AdaptiveQualitySample` 32→24, `OverlayPlacement` 48→40 and `PaintData` 536→528 (520 after #734) by ordering members widest-first. The other padding rows are **accepted**: `KeyframeTiming` (64), `AnimationEntry` (216), `BidiVisualRun` (24), `PaintImageStyle`/`PaintVectorStyle` (56), `PaintOutsets` (20) and `TextConstraints` (8) carry only *tail* padding, which no reorder can remove | `padding_size` test (`static_assert`), `sizeof_pace_decision`, `sizeof_adaptive_quality_sample`, `sizeof_overlay_placement` and `sizeof_paint_data` rows |
| Resident bytes per plain node (20k-node tree) | 437 on Windows x64 (364 when first recorded), 327 on Linux x64 (when recorded), budget 1024 | `memory_steady_state` test, `tests/golden/memory/steady_state.txt`, `resident_bytes_per_node` row (warn) |
| Idle cycles after warm-up | 0 bytes of RSS growth measured, budget 2 MiB | `memory_steady_state` test, `node_dirty_scratch_bytes` row (warn) |
| Headless generated-UI sample | build and layout about 0.6 MB, first paint about 0.9 MB, 300 click + repaint cycles 0 to 45 KB, process peak about 8.7 MB (budgets 8 MiB, 8 MiB, 1 MiB and 64 MiB) | `generated_ui_memory` test, `tests/golden/memory/generated_ui.txt` |

#### The memory ratchet

The rows marked with a bare name are pinned in
[`tests/golden/perf/memory_budget.json`](../../tests/golden/perf/memory_budget.json) and ratcheted by
`scripts/check_memory_budget.py`, the same two-sided policy as the preview budget: an entry fails when
it is worse than its budget by more than `tolerance.worse` **and** when it is better by more than
`tolerance.better`, because an unrecorded improvement means the budget went stale. `--update` re-pins
a tightening and refuses to loosen any entry.

The rows are split by **reproducibility, not importance**, and that is what makes the gate
trustworthy:

- **`enforce` (0%/0%) -- sizes and allocation counts.** `sizeof` of a hot type is fixed by the ABI,
  and an allocation count comes from a replaced global `operator new` in one process. The measurement
  module runs the probe twice and fails the row if the two values differ, so a 0% band is a real
  invariant rather than noise cover. These are the rows a regression arrives through: a node that
  grows 16 bytes costs 160 KB on a 10k-node screen and shows up nowhere else. Two qualifications,
  both recorded rather than papered over. The budget pins one **build configuration** (Release), and
  the gate refuses to compare a measurement from another one: MSVC Debug measures a larger `sizeof(Node)`,
  `sizeof(PaintData)` and `paint_slot_bytes` and two allocations for a plain node against
  Release's 320/520/528/1, so a Debug number is a different claim, not a regression. And
  `sizeof(ComputedStyle)` is deliberately *not* pinned: it is a `std::map` handle, 16 bytes on MSVC
  and 48 on libstdc++, so pinning it would record a standard-library difference as a memory budget.
  The portable per-node style cost is the recorded first-insert size (128 bytes on both lanes, 208 in
  MSVC Debug to match the configuration rule).
- **`warn` -- resident memory.** RSS is an OS reading of a heap: it moves with page granularity,
  allocator behaviour, ASLR and machine load, and the two lanes disagree by double digits (437 vs 327 bytes per
  node in the last recorded runs) with an identical `sizeof(Node)` of 320. A gate that fails at random gets deleted, and
  deleting it is worse than the trend line, so those rows are measured and reported on every run
  inside a wide band and never block. The generous bound that *does* block a leak is in the CTest
  tests above (`memory_steady_state`, `generated_ui_memory`).
- **`unmeasured` -- declared, with a reason and no number.** Android PSS cannot be measured by this
  repository's lanes, and the validator rejects a budget on an
  `unmeasured` entry: a plausible number for a quantity nobody measured reads as evidence and
  ratchets a fiction. See the next two paragraphs.

Android PSS is **not measured anywhere in this repository**, because there is no Android sample and
no emulator job (V1 targets Windows and Linux; Android is advisory and post-V1). It is the row that
would show DPR-squared bitmap scaling, and it stays declared-but-unmeasured with that reason rather
than carrying an invented figure.

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

The **VMA category split is ratcheted on lavapipe**: the null backend reports zero allocator bytes by
design, so `gpu_memory_probe` runs the Vulkan backend on lavapipe and the `enforce` rows
`gpu_vma_category_bytes` and `gpu_category_peak_after_free_bytes` pin it (2 195 456 bytes each when
recorded); `rhi_memory` asserts the per-resource costs. The swapchain category is driver-owned and not
counted at all. Real-GPU block growth is not measured by this (spikes of 60-93 ms in `create_texture` on two AMD GPUs are
tracked on #697). The Vulkan device now times the three slow steps of every `create_texture`
(`vulkan::texture_create_timings`: `vmaCreateImage`, the staging memcpy, `submit_upload`), and
`cpu_to_gpu_4k_measure` prints them. On an RTX 4060 (Windows, Release) there is no spike at all:
4K `vmaCreateImage` averages 0.12 ms (max 2.4 ms) and the staging write 0.7 ms, so the tail is an AMD
behaviour and the policy of #697 cannot be validated on this vendor.

A node carried 1984 bytes before the rarely used tables became lazily allocated, so a 10k-node
screen of plain views drops from about 20 MB to about 3 MB of node storage (Yoga node and style
storage are extra). Clearing a table keeps its allocation, so toggling a value never reallocates.
`diagnostics::sample_process_memory()` (`diagnostics/memory.hpp`) reads resident and peak bytes
with no heap allocation (VmRSS/VmHWM on Linux and Android, the working set on Windows,
`resident_size` on macOS and iOS); `MemoryGauges` publishes them as the gauges `memory.rss_bytes`
and `memory.rss_peak_bytes`. It is a syscall: sample per second or per scripted checkpoint, never
per frame. Real RSS is machine dependent, so the committed bounds are generous and catch leaks and
order-of-magnitude regressions, not noise. Raise a budget only with a recorded decision. The
`generated_ui_memory` test runs the compiler-generated demo headless (CPU canvas, no GPU), samples
resident memory after the build, the first paint and a scripted click loop, and prints the numbers on
every run so CI logs show the trend; it fails above the committed bounds.

`scripts/memory_budget_measure.py` runs `tests/benchmarks/memory/memory_sizes_probe.cpp` (CMake
target `memory_sizes_probe`) in a fresh process per sample and parses its `name=value` lines:
`sizeof` and allocation counts are taken exactly (and must agree across two runs), resident bytes as
the minimum of five runs, because contention can only ever *add* resident bytes. The probe is also
the cheap `memory_sizes` CTest; `memory_budget` runs the checker against it in any lane that has a
build tree. `scripts/gates.py` runs `--no-build`, which validates the budget file's shape in the
aggregator job, where there is no build directory -- the same split as the preview budget, which
runs in its lane rather than in the aggregator.

**Allocations per frame** are a second ratchet, `tests/golden/perf/alloc_budget.json`, checked by
`scripts/check_alloc_budget.py` (issue #701, [ADR-070](../../decisions/ADR-070-frame-arena-not-pmr.md)):
the mean allocation count of one steady-state frame per scene and scenario, from a replaced global
`operator new`, with one table per compiler lane because MSVC and libstdc++ allocate differently. A row
may fall (`--update`) and may not rise. The current numbers and the gap to the spec's "zero in a steady
frame" are in [`performance-cpu-benchmark.md`](performance-cpu-benchmark.md).

Cache-line layout - false sharing between counters that different threads write, and hot structs
that straddle a line - is its own note: [`cache-lines.md`](cache-lines.md).

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

### 6.1 Implemented decision layer, its render-target consumer (step 2) and the present cap (step 3)

The decision layer, the step-2 render-target consumer and the step-3 present cap are implemented. A
downgrade now changes pixels and the present rate: the window renders the scene into a smaller target,
resamples it up on present, and lowers the pacer's cap to the next refresh divisor. Step 1 (effects)
and step 4 (pause non-essential animations) are implemented as the two ends of the ladder (see the decisions below); step 5 (report) is as well, so all six ladder positions are live.

**Decisions for steps 1 and 4 ([#147](https://github.com/thnak/Tessera/issues/147)), implemented as described.** An *optional effect* is a member of a fixed paint-kind allowlist (drop shadows, blurs, gradient oversampling); the allowlist is runtime-owned, adds no CSS surface and does not reuse `prefers-reduced-motion`, which keeps its accessibility meaning. A *non-essential animation* is one that animates only paint-only properties; `transform` and `opacity` keep running. Pausing freezes the animation at its current value, resumes from there when quality recovers, does not keep the loop awake while frozen, and is counted in a `FrameDiagnostics` counter. Each step fires only after the steps above it, and recovery restores it. The controller side is implemented: `quality_step` is the ladder position, `AdaptiveQualitySample::effects_reduced` (position >= 1) and `animations_paused` (last position) are set at every later position and clear on recovery, and `FrameDiagnostics` counts both flips (`adaptive_effect_changes`, `adaptive_animation_pause_changes`). `effects_reduced` is consumed: `PaintContext::reduce_effects` (set by `FramePipeline::set_reduce_effects`, driven by `AppHost` on a flip, which also requests a full repaint) caps a blurred box-shadow at `kReducedBlurLayers` (2) layers with the same box and reach, so damage stays valid; the flag is part of the paint-cache key, so recovery re-records the full stack. Blurred box-shadow is the only member of the allowlist today (the renderer has no other effect). `animations_paused` is consumed too: `AppHost` calls `AnimationRuntime::set_suspend_non_essential` on a flip and wakes the loop. An entry is non-essential (`AnimationEntry::non_essential`, computed at `start()` by `is_non_essential_animation`) when every animated property is paint-only and neither `transform` nor `opacity`; mixed animations are essential. A suspended entry keeps its elapsed time (it resumes where it stopped), stops counting toward `keep_awake`, and is separate from the app's own `paused` flag, so recovery never resumes an app-paused animation. Springs are not classified and keep running. Steps 1-5 of the ladder are now all implemented. **Empty stages are stepped over.** A ladder position with nothing to act on would cost a whole miss run and cooldown for no relief, so `AdaptiveQualityController::observe` takes an `AdaptiveQualityStageProbe` and, on the observation that already changes position, steps over an effects position when no node has a blurred outer shadow (`Node::PaintOutsets::blurred`, set by the style sync) and steps back over it on recovery, all inside one change and one cooldown. The probe is read only at that moment, never per frame, so a shadow appearing or vanishing cannot cause a change by itself. The pause position is still entered when no non-essential animation exists (it is free, and an animation started later is frozen at once); `AnimationRuntime::non_essential_active` is its probe. A scene with no blurred shadow therefore reaches the first `renderScale` step after one miss run, as before the ladder grew its ends.

`src/runtime/adaptive_quality.hpp` is a pure state machine fed one frame observation at a time
(frame time in ms plus the `adaptiveQuality`, `powerPolicy`, `renderScale`, `minRenderScale`,
`frameBudgetMs`, `maxFps` and display-refresh fields). It is deterministic — no clock, no RNG, no
locale, no allocation, not even at construction (the type is trivially copyable) — so an observation
sequence always yields the same decision sequence. It stays the **only** hysteresis authority: the
consumers read `AdaptiveQualitySample::render_scale` and `AdaptiveQualitySample::present_cap_hz` and
never re-implement a threshold, a step or a cooldown.

| Constant | Value | Why |
|---|---|---|
| Downgrade run | 4 consecutive missed budgets | one slow frame is noise; four at 60 Hz is ~66 ms of sustained overrun |
| Upgrade run | 12 consecutive met budgets | recovery is deliberately slower than the downgrade, so a borderline machine does not ping-pong |
| Cooldown | 8 observed frames after a change | no two changes within it: at most one step per ~133 ms at 60 Hz |
| Ladder | 6 positions: 1 reduces optional effects, 2-5 walk `renderScale` to `minRenderScale` (4 evenly spaced steps) and the present divisor, 6 pauses non-essential animations | ADR-019 §8 order; bounded, no fractional creep; `AdaptiveQualitySample::quality_step` is the position |
| Present-cap floor | 10 Hz | below it an interactive UI stops being interactive; step 4 is the right lever there |

Each change moves exactly one ladder step, clamped to `[minRenderScale, renderScale]`;
`minRenderScale == renderScale` leaves no room to move, so it never changes. `adaptiveQuality=false`
and `powerPolicy=benchmark` disable the controller completely. Cooldown frames are not evidence for
the next run, so a sustained overload waits out the cooldown and then a full downgrade run, and an
alternating miss/met sequence changes nothing.

The frame path feeds one observation per presented frame: `AppHost::observe_frame_time_ms`, called
by `WindowHost`. The sample's source is the device's GPU timestamp probe: when
`rhi::Device::timestamp_capability()` reports timestamps usable on the frame-submission queue
family, the controller sees the GPU interval of a completed submission; otherwise (the null
backend, a device without timestamp queries, a zero or non-finite period) it sees the frame's CPU
cost through present. The probe is the only authority — no layer above the RHI infers the answer
from a backend name. A change is recorded in `runtime::FrameDiagnostics` (`adaptive_quality()`,
`adaptive_quality_changes()`, `adaptive_quality_downgrades()`, `adaptive_present_cap_changes()`), so
a downgrade is visible without sampling frame time, and it survives `AppHost::resize()`, which only
re-creates the pipeline.

**The GPU frame-time signal.** `AppHost::observe_frame_time_sample(cpu_ms, gpu_ms)` keeps a single
observation path: `observe_frame_time_ms()` is the CPU-only entry point and delegates with no GPU
value, so every existing caller and test is unchanged. The interval is recorded in the frame's own
command buffer (`TOP_OF_PIPE` at the first command, `ALL_COMMANDS` just before it ends). The
Vulkan device owns one bounded ring of timestamp pairs (`kGpuTimestampSlots = 8`) allocated once
with the device; a submission uses `slot % 8`, and `WindowHost` reuses a frame slot only after the
previous submission on it reported completion through the existing non-blocking
`CommandList::is_complete()` poll. The pair is therefore read only for a completed submission,
with `vkGetQueryPoolResults` and no `WAIT_BIT` — a host query, never a fence wait or
`vkDeviceWaitIdle` (ADR-021). A value is unreadable until its submission completes, so the sample
lags at most two frames; `CommandList::gpu_frame_time_ms()` converts it with the device's
`timestampPeriod` and rejects a zero, a 64-bit wrap and an absurd (> 10 s) delta. The selection
rule is pure and tested with a fake capability (`tessera/runtime/frame_time_source.hpp`): a
present and plausible GPU interval wins, anything else keeps the CPU time.

`FrameDiagnostics` records the source of every observation — `frame_time_source()`,
`gpu_frame_time_samples()`, `cpu_frame_time_samples()` — and `WindowFrameStats` reports
`frame_time_source`, `gpu_frame_time_samples` and `gpu_frame_time_fallbacks`, so the first frames
after startup and any skipped or stale read are visible as fallbacks instead of being silently
reported as GPU numbers. The controller's thresholds, ladder and actions are unchanged; only the
number it compares moved.

**The present cap (step 3).** `src/runtime/present_cap.hpp` is the pure rule, separate from the
hysteresis state machine so it is testable without a window, a pacer or a clock:

```
cap(refresh, maxFps, step) =
    step 0        -> maxFps                                  (unchanged)
    refresh <= 0  -> maxFps                                  (no divisor exists)
    otherwise     -> the next refresh divisor below the previous step's effective rate,
                     holding at the last divisor that is >= 10 Hz
```

The effective rate at step 0 is `min(maxFps, refresh)` when `maxFps` is non-zero, else the refresh
itself, so a step never raises the rate above `maxFps` (the hard ceiling of
[`runtime-config-v1.md`](../specs/runtime-config-v1.md) section 5.2) or above the display refresh.
A cap that is already a divisor moves to the *next* one (`refresh/2` -> `refresh/3`), never to
itself. On a 60 Hz display with no cap the sequence is 60, 30, 20, 15, 12 Hz; on 144 Hz it is 144,
72, 48, 36, 28.8 Hz. The floor stops the walk rather than clamping to a rate the display cannot hit,
so every value after a downgrade is a real `refresh/k`. An upgrade restores the previous step's cap
because the cap is the same pure function of the step — there is no separate "previous divisor"
state that could drift from the ladder.

**How it is applied.** `WindowHost` calls `apply_present_cap()` after each frame observation, which
feeds the decided cap to `FramePacer::update()` — the pacer's existing rate-change path, the same
mechanism a scripted refresh change uses. `update()` resyncs the next deadline to one new interval
after the change instant, so the cap takes effect at the next frame boundary with no burst of
catch-up frames, no stall at the old cadence, no blocking wait and no swapchain recreation. A cap
that did not move is a no-op, so the running deadline is untouched. The display refresh comes from
the HAL query (`hal::Window::display_refresh_hz()`, resolved once into
`WindowOptions::refresh_hz`; an explicit non-zero option wins, 0 = unknown), as specified in
[`runtime-config-v1.md`](../specs/runtime-config-v1.md) section 6.2. With no refresh there is
no divisor, so the cap stays at `maxFps` and the pacer keeps its documented deadline pacing
([`runtime-config-v1.md`](../specs/runtime-config-v1.md) section 5.2) — the fallback is the existing
behaviour, not a new one. Diagnostics: `WindowFrameStats::present_cap_hz`,
`present_interval_seconds` and `present_cap_changes`.

**What the consumer scales.** One value: the extent of the target the scene renders into. With the
window's presented (physical) pixel extent `P` and the decision `s = renderScale`, the target is

```
target = max(1, floor(P * s)) per axis
```

and the present resamples that target up to `P` with a filtered blit. `P` is the window's logical
extent times the device scale, rounded up to whole pixels, so the adaptive multiplier is a resample
of exactly what the window already presented. The base is the *presented* extent rather than the
unrounded logical × device product on purpose: the physical grid is a whole number of pixels, and
using it makes `s = 1.0` exactly identity on every DPI, including fractional ones. The two axes are
computed (and can round) independently, and the content transform the paint uses is the exact
extent ratio `target / P`, so the resample always covers the whole image.

**What it never touches.** The device scale (the monitor-derived value quantized onto the render-scale
ladder, [`window-resize.md`](window-resize.md) §2.3), and therefore everything keyed by it: glyph
rasterization, the text atlas, and caret/metrics values. Layout does not change either — the scene is
laid out, style-resolved and painted in CSS pixels exactly as before; only the mapping from those
coordinates to the smaller target changes. The frame's content scale composes this adaptive ratio
with the window's device scale (`ui::scale::content_scale_for`, `display-aware-rendering.md` §6), so
a downgrade only multiplies the device factor by the quality factor. The window and its swapchain
keep the physical size, so a quality change never resizes the OS window or recreates the swapchain.

**When it applies.** At the next frame boundary: the multiplier is read from the decision in force
when the frame begins, the target is (re)created only when the decided extent changed, and the
resample is recorded in the same submission as the paint. There is no mid-frame change, no per-frame
allocation in the steady state, and no GPU wait.

**The bit-identical guarantee at scale 1.0.** When `s = 1.0` the target extent equals `P`, so the
frame takes the existing direct path — no offscreen target is created and no resample runs — and the
presented image is byte-for-byte the full-quality frame the tree produced before this consumer
existed. The committed goldens and the frame-pipeline/app-window-capture tests therefore pass
unchanged, and they are the acceptance for this slice.

The decision is also observable per frame: `WindowFrameStats::render_scale`, `target_extent`,
`present_extent`, `scaled_frames` and `scale_fallbacks` in [`window.hpp`](../../include/tessera/app/window.hpp).
A backend whose swapchain images cannot be resampled reports `supports_scaled_present() == false` and
the driver stays on the direct path instead of dropping frames.

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

### 9.1 CPU benchmark and the render-thread trigger

The GPU-free `tessera_benchmark` (`cpu_benchmark`, issue #241) drives the production headless frame
path on a deterministic 12 000-node scene; budgets live in `tests/golden/perf/cpu_benchmark.txt` and
allocations per frame in `tests/golden/perf/alloc_budget.json` (#701). The full write-up, the current
numbers and the history of each cull and style optimization are in
[`performance-cpu-benchmark.md`](performance-cpu-benchmark.md). Summary of the 2026-11-24 run (MSVC
Release, 12 000 nodes, median): first style + layout 222.6 ms, layout + paint after one change 15-19 ms,
damaged-frame paint 0.17 ms (6 nodes), **full paint 5.7 ms** (232 visible nodes; it was 89.6 ms until
#257 made a full repaint cull to the viewport, on by default). The render-thread trigger
(`paint_time_ms` above about 4 ms or 30% of `frameBudgetMs` at 10k nodes) is marginally exceeded, and
only for a scene whose 12 000 nodes are all visible would a full paint still cost the whole tree. The V1
design (single UI thread, no worker recording) is unchanged.


## 10. Open questions

- Per-platform or battery-state overrides of `backgroundMaxFps` (the default is `0` on all platforms per [`runtime-config-v1.md`](../specs/runtime-config-v1.md); no per-platform override is decided).
- Thermal/power-source signals and whether to use them at all (no decision yet; HAL `PowerState` in [`hal.md`](hal.md) exposes them).
- ~~Per-monitor render scale and mixed-DPI multi-window.~~ Decided: one render scale per window in
  V1, derived from that window's `WindowMetrics` and recomputed only on a DPI or monitor change
  ([`window-resize.md`](window-resize.md) §2.3). Per-monitor scale is deferred until multi-window
  or per-display rendering is scheduled or a mixed-DPI app needs it.
- How much adaptive quality is acceptable without visual regression.
- Benchmark harness and baseline budgets per scene class: the harness, the CPU budget file, the
  allocation ratchet and the memory ratchet exist for the one reference scene; per-scene-class budgets
  (animated, scrolling, text-heavy) exist only for allocations so far.
- ~~Should `SceneStyleTree::compute` memoize per style generation?~~ Decided and implemented (#711,
  `SceneStyleTree::begin_memo`/`end_memo`): one cascade per node per generation.
- ~~Should a full paint cull against the viewport by default (#257)?~~ Decided: yes, on by default
  (`cull_paint`), full paint 89.6 -> 5.7 ms on the reference scene. Open remainder: a screen where all
  12 000 nodes are visible still paints the whole tree, which is where ADR-021's worker recording
  would have to be re-opened.
- Which of the open V1 performance items are release blockers: #842 (a worker completion wakes the parked UI thread on Linux and Windows; the Android advisory
  lane still waits out the clamp), #697, #476 (retained draw
  list and layer cache; `layerCacheBudgetMb` is parsed but unused) and #683 (GPU vector rasterizer, a
  spike with no ADR yet).
