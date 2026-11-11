# Runtime loop, frame pacing, and display refresh

Status: design for Big Step B2 (runtime slice, rendered offscreen) and B3 (window/swapchain path); both
are defined in [AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step). The
deterministic fixed-step accumulator is implemented in `src/runtime/fixed_step.*`, with a clock
interface and deterministic `ManualClock` in `src/runtime/clock.*`, and a `RuntimeLoop` with
render-on-demand/wake state in `src/runtime/runtime_loop.*`, a deadline-based `FramePacer`
(`src/runtime/frame_pacer.*`), the HAL steady clock + bounded wait (`src/hal/timer/`),
`runtime::SystemClock`, and the Windows HAL event wait (`hal::Window::wait_for_events`,
`hal/window_wait.hpp`). The windowed `batched2d_poc` runs these together: `RuntimeLoop` on
`runtime::SteadyClock` decides whether to render, `wait_for_next_frame` sleeps to the pacer deadline,
and frames are presented asynchronously through a swapchain. `batched2d_poc --on-demand` runs the
render-on-demand variant: a `Node` tree is dirtied, `FramePipeline` derives damage, and the loop
parks on the window wait and presents nothing while the scene is clean. Still pending: real input
driving the scene (the HAL translates none yet), and the completion-queue and animation-bridge
wiring in that loop.
Normative
terms are used by [`runtime-config-v1.md`](../specs/runtime-config-v1.md) (runtime config keys and
defaults). Design basis: [ADR-017](../../decisions/ADR-017-input-and-fixed-step-runtime.md) (input and
fixed-step runtime), [ADR-018](../../decisions/ADR-018-dirty-rendering-and-frame-pacing.md) (dirty
rendering and frame pacing) and [ADR-019](../../decisions/ADR-019-power-aware-runtime.md) (power-aware
runtime defaults). Other terms are in [`glossary.md`](../reference/glossary.md) and section 1 below.

## 0. Runtime pipeline

```text
platform input (hal/input)
  -> bounded event queue
  -> fixed tick:
       sample input            [input.md]
       update(dt)              hooks, animations, data streams
       resolve dirty styles    [stylesheet-v1.md]
       layout dirty subtrees   [ADR-005, Yoga layout adapter]
       paint dirty subtrees    [dirty-rendering.md]
  -> render if dirty/continuous
       record draws, submit, present
  -> pacing: maxFps / vsync / VRR / frame budget
```

The rest of this document defines the tick/render split, pacing, and display-refresh policy.
Dirty work is defined in [`dirty-rendering.md`](dirty-rendering.md); input in
[`input.md`](input.md); styles in [`stylesheet-v1.md`](../specs/stylesheet-v1.md); resize in
[`window-resize.md`](window-resize.md); async completion in [`async.md`](async.md); measurement
in [`diagnostics.md`](diagnostics.md); animation/overscroll in
[`animation-v1.md`](../specs/animation-v1.md).

## 1. Terms

| Term | Meaning |
|---|---|
| **Simulation tick** | One fixed-dt update. Deterministic. |
| **Render frame** | One layout/paint/render/present cycle. Rate may differ from ticks. |
| **Present** | Handing a finished image to the display/swapchain. |
| **Refresh rate** | Display scan-out rate in Hz (60, 120, 144, 240, VRR range). |
| **Frame cap** | Maximum presented frames per second (`maxFps`; `0` = uncapped). |
| **Frame budget** | Target wall time per presented frame. |
| **Frame pacing** | Scheduling presents to hit a stable cadence/deadline. |
| **VRR** | Variable refresh rate (FreeSync/G-Sync/HDMI VRR). |
| **Damage** | Region of the target that changed and must be redrawn. |
| **Golden** | Reference image produced by the deterministic software path (see [`golden-tests-v1.md`](../specs/golden-tests-v1.md)). |
| **HAL** | Hardware/OS abstraction layer ([`hal.md`](hal.md)); `hal/input` in the pipeline above is its input part. |

## 2. Invariants

1. Simulation advances in fixed steps; rendering may run at a different rate.
2. No wall-clock reads inside `update`, style resolution, layout, or paint.
3. Input is sampled once per simulation tick; the renderer never reads a live input device.
4. Order per tick: `sample input -> update -> style dirty -> layout -> paint -> render`.
5. Style, layout, paint, and render work is proportional to dirty state, not tree size.
6. Golden/headless mode uses a simulated clock and a fixed dt; it never waits for vsync.
7. A slow frame never causes unbounded catch-up. The loop clamps frame delta and update count.
8. A static screen performs no presents and no GPU submissions after it settles. Continuous
   rendering is an explicit mode, not the default.
9. The loop never busy-waits and never targets full GPU utilization. See
   [`performance.md`](performance.md).

## 3. Loop

```text
fixed_dt = 1 / fixedTimestepHz
accumulator = 0
previous = clock.now()

loop:
    frame_dt = min(clock.now() - previous, maxFrameDeltaMs / 1000) * timeScale
    previous = clock.now()
    accumulator += frame_dt

    steps = 0
    while accumulator >= fixed_dt and steps < maxUpdatesPerFrame:
        sample_input()          // drain queue, snapshot pointer/key state
        flush_scheduled_updates() // setter-dirtied components: update_effects + apply_state
        update(fixed_dt)        // hooks, animation state, data streams
        resolve_dirty_styles()
        layout_dirty_subtrees()
        paint_dirty_subtrees()
        accumulator -= fixed_dt
        steps += 1

    if steps == maxUpdatesPerFrame and accumulator >= fixed_dt:
        drop_backlog()          // keep only a bounded remainder; report a spike
        accumulator = min(accumulator, fixed_dt)

    alpha = accumulator / fixed_dt
    if should_render(alpha):
        render(alpha)           // interpolate visual state, submit, present
```

`maxFrameDeltaMs` (default 250) prevents the spiral of death after a debugger pause or OS
stall. `maxUpdatesPerFrame` (default 5) bounds catch-up work. Both are configurable.

### 3.1 Simulation vs render rate

- Simulation: fixed `fixedTimestepHz` (default 60). Deterministic.
- Render: as fast as the display allows, or capped by `maxFps`.
- Interpolation: `alpha` blends the previous and current visual state for animation/render only.
  Simulation state is never mutated by the renderer.
- Discrete state (visibility, focus, text content) is not interpolated.

#### 3.1.1 Paint-only render interpolation

Implemented as a paint-only view between two simulation ticks. V1 interpolates **transform,
opacity and color** and nothing else: layout boxes, layout-feeding scroll offsets, text advances,
visibility, focus and text content stay discrete per tick, so a render-rate frame can never move a
box or make hit testing disagree with layout ([ADR-018](../../decisions/ADR-018-dirty-rendering-and-frame-pacing.md)
keeps the scene bounds authoritative). The open-questions register's `design/game-loop.md#2` row is
the scope decision.

- **Alpha.** `runtime::interpolation_alpha(accumulator, fixed_dt)` (in `src/runtime/fixed_step.*`)
  is the single definition: the fraction of one fixed step elapsed since the last tick, clamped to
  `[0, 1]`. It is 0 when a render coincides with a tick (the accumulator was just drained), 0.5
  mid-step, and 1 as the next tick comes due. Non-finite or non-positive input yields 0 instead of
  extrapolating from a broken clock. `FixedStepLoop::alpha()` and `FixedStepResult::alpha` use it.
- **Samples.** Each tick captures the paint-visible declarations of its animation samples
  (`ui/frame_interpolation.hpp`, `PaintSamples`): `transform` (resolved to a matrix around the
  node's box origin, using the node's current box for percentages and `transform-origin`),
  `opacity`, `color`, `background-color` and the four `border-*-color` values. A sample containing
  only layout/text properties produces no entry, so a `width` or `font-size` animation is provably
  invisible to interpolation. Declarations are typed at capture time (numbers, sRGB colors,
  matrices), not re-parsed from strings later.
- **Blend.** For every field present on the current tick, the render paints
  `lerp(previous, current, alpha)`: alpha 0 paints the previous tick's sample (the value on screen
  just before the tick, so motion is continuous across the boundary), alpha 1 the current one, and
  a node sampled for the first time keeps its current value instead of blending from nothing. A
  property the current tick no longer samples is dropped, not held, because the caller's style
  provider owns the reverted value. Transforms blend as translate/scale/rotate components, with
  rotation taking the shorter arc; a matrix that has no such form (a skew, or a `matrix()` with a
  shear) holds the current tick's value instead of wobbling through a wrong shape. Colors blend in
  sRGB, like the animation sampler.
- **Seam.** `FramePipeline::tick(root, dt, alpha)` stores the tick pair; `interpolation_view()`
  returns an `InterpolationPaintView`, a read-only `paint::PaintView`. The paint pass applies it to
  a local copy of the computed style and to the matrix it paints with (`paint_pass.hpp`), so the
  scene, dirty flags, damage, layout and hit testing never see it. `paint_frame()` applies the view
  automatically. Passing alpha is the opt-in: the two-argument `tick(root, dt)` keeps painting the
  authoritative state, so a caller that does not drive the render rate is unaffected. On top of
  that, `FramePipelineConfig::interpolation` (default `true`, mirroring `animationInterpolation` in
  [`runtime-config-v1.md`](../specs/runtime-config-v1.md)) is the global switch that deterministic
  and golden runs turn off (ADR-018 rule 11).
- **Dirty and damaged work.** Interpolation itself is not a wake source: it applies to a frame that
  paint/render damage or an explicit frame request already made due, and changing alpha alone
  produces no presents. The sampled properties are paint/render work only — `transform`, `opacity`
  and every `*-color` (including `border-*-color`) map to `Paint | Render`, never `Layout` — and a
  settled scene still idles with zero presents. Because the interpolated box can fall between the
  two tick positions (a box moving more than its own width per tick, a rotation whose intermediate
  AABB is wider than either endpoint), `finish_frame()` adds each transform-sampled node's painted
  box to the damage, so a damage-clipped repaint cannot leave stale pixels.
- **Determinism.** No wall clock, no random ids: the same tick script and the same alpha paint the
  same commands on every platform (`frame_pipeline_style_sync` / `frame_pipeline_overflow` tests,
  plus the sample-model tests in `tests/unit/frame_pipeline_style_sync_tests.cpp`).

### 3.2 Render-on-demand

Two policies:

- **Continuous** — render every present opportunity (video, live charts, animations).
- **On-demand** — render only when a wake source fires:
  - input event
  - style/layout/paint/render dirty
  - active animation/timer
  - external data update
  - async task completion (visible resource ready)
  - resize/DPI/theme change ([window-resize.md](window-resize.md))
  - explicit `request_frame()`

`renderOnDemand` defaults to `true` (`powerPolicy=adaptive`): static screens idle, while active
animations, input, timers, and data streams are wake sources and still render on schedule.
Continuous/video/benchmark modes opt out explicitly (`renderOnDemand=false`). In on-demand mode
the loop still ticks only when needed; idle CPU/GPU usage approaches zero.

`should_render(alpha)` returns true if any of:

```text
continuous_mode
or render_dirty
or active_animations
or pending_damage
or explicit_frame_request
or alpha changed enough to matter for interpolation
```

The threshold for "alpha changed enough" is not specified.

A wake source must be edge-triggered; the loop must not poll devices or the filesystem.

## 4. Frame cap and frame pacing

### 4.1 `maxFps`

- `0` = uncapped. The display/vsync remains the practical limit.
- `N > 0` = never present more than `N` frames per second.
- If `vsync` is on and `N < refresh`, the pacer uses a **refresh divisor** when possible
  (for example 144 Hz display, `maxFps=72` -> present every second refresh).
- If `N` is not a divisor, the pacer uses a deadline schedule and accepts occasional jitter.
  The schedule keeps its cadence when a frame finishes a little after its deadline (the next
  deadline is one interval after the old one); it only resyncs to `now` after a whole interval
  was missed. Resyncing on any lateness made every period `interval + render time`, so a 60 fps
  cap ran at about 50 fps. On Windows the bounded wait uses a high-resolution waitable timer, so
  a frame-sized wait does not round up to the 15.6 ms system tick.
- `maxFps` must not be implemented with a busy-wait. Use the platform waitable timer /
  `vkWaitForPresentKHR` / display-link callback / `sleep_until` with a margin.

Effective present rate:

```text
if maxFps == 0: effective = refresh (or VRR content rate)
else:           effective = min(maxFps, refresh)
```

`maxFps` is a **present-rate cap**, not a simulation-rate cap. The simulation always runs at
`fixedTimestepHz` (scaled by `timeScale`).

### 4.2 Frame pacing modes

| Mode | Behavior | Use |
|---|---|---|
| `auto` | Pick `smooth` for continuous, `low-latency` for input-driven UI | default |
| `smooth` | Stable cadence; queue one frame; tolerate small jitter | dashboards, video |
| `low-latency` | Present as soon as ready; minimal queue | tools, pointer-driven UI |
| `power-save` | Prefer lower present rate when content is static; allow longer sleeps | kiosks, battery |

`latencyFrames` (1–3) bounds swapchain/queue depth. `1` is lowest latency; `2` is the default
(`latencyFrames` in [`runtime-config-v1.md`](../specs/runtime-config-v1.md)). The runtime never queues more than `latencyFrames` unfinished frames.

### 4.3 Deadline scheduling

For a target rate `R`:

```text
deadline_n = start + n / R
sleep until deadline_n - safety_margin
render and present
```

The pacer measures present latency and adjusts the margin. It reports:

- `frame_time_ms`, `present_time_ms`, `sleep_time_ms`
- `missed_deadlines`
- `effective_fps`
- `refresh_hz`, `vrr_active`

If the renderer misses the deadline repeatedly (the miss count/window is not specified; see
[`performance.md`](performance.md) for budgets), the runtime may:

1. reduce optional effects,
2. reduce `renderScale` toward `minRenderScale`,
3. drop to the next lower refresh divisor,
4. pause non-essential animations,
5. report a frame-budget violation.

It must not silently reduce simulation fidelity or input responsiveness.

### 4.4 Power and utilization

Tessera is an application framework, not a benchmark. The fixed timestep exists for
determinism; it does **not** mean the GPU is driven continuously.

- Default policy `adaptive`: on-demand rendering, dirty work, capped present rate.
- Static content: zero presents, zero GPU submissions, no active animation ticks.
- Occluded/minimized: paused by default (`pauseWhenOccluded`, default `true` in [`runtime-config-v1.md`](../specs/runtime-config-v1.md)).
- Continuous mode is explicit for video/live dashboards (`renderOnDemand=false`,
  `powerPolicy=performance`).
- `benchmark` mode (uncapped/full redraw) is diagnostic only and never a production default.
- Adaptive quality reacts to missed frame budgets, not to a "GPU utilization %" target.

Hardware utilization is determined by the dirty set, damage area, render scale, effects,
present rate, frames in flight, and visibility. See [`performance.md`](performance.md) for the
full control matrix and budgets.

## 5. Vsync and present modes

| `vsync` | Vulkan present mode | DXGI/other | Notes |
|---|---|---|---|
| `off` | `IMMEDIATE` (if allowed) | tearing allowed | lowest latency; may tear |
| `on` | `FIFO` | sync interval 1 | stable, one frame queue |
| `adaptive` | `FIFO_RELAXED` | adaptive vsync | tears only on late frames |
| `mailbox` | `MAILBOX` | flip model mailbox | replaces queued frame; low latency |

Rules:

- `vsync=off` requires `allowTearing=true` on platforms that gate tearing.
- `mailbox` falls back to `FIFO_RELAXED`, then `FIFO`, if unsupported.
- `adaptive` falls back to `FIFO`.
- The selected mode is reported once at startup; capability changes (window move, display
  change) re-query and fall back safely.
- The runtime never assumes a present mode exists. It queries surface/swapchain capabilities.

## 6. Dynamic refresh rate and VRR

`dynamicRefreshRate` controls whether the display may follow the content rate.

| Value | Behavior |
|---|---|
| `off` | Use the preferred/fixed refresh rate; no VRR negotiation. |
| `auto` | Use VRR when the display/OS exposes it; otherwise present at the preferred rate. |
| `preferred` | Request `preferredRefreshRate` (for example 120) when available; fall back to the desktop rate. |

Policy:

1. Query the display refresh range and VRR capability (platform-specific).
2. Query surface present modes.
3. If VRR is available and `dynamicRefreshRate=auto`:
   - use `mailbox` or `FIFO_RELAXED` as the pacing mode;
   - present when the frame is ready;
   - do not add a fixed-rate sleep that fights VRR;
   - still honor `maxFps` if it is non-zero.
4. If VRR is not available:
   - fall back to `FIFO`/`FIFO_RELAXED` at the preferred refresh;
   - use the frame pacer with a refresh divisor.
5. Static content in on-demand mode stops presenting; VRR idles.

Platform notes:

- **Windows**: query display modes/refresh via DXGI; use a flip-model swapchain with
  `ALLOW_TEARING` when `vsync=off` and the OS permits it. VRR is managed by the driver/OS; the
  app presents at content rate. Use a waitable swapchain for low-latency pacing.
- **Linux/X11**: query CRTC/output refresh via RandR; use the Present extension for timing and
  damage. VRR depends on the driver/compositor.
- **Linux/Wayland**: use `wp_presentation` feedback and frame callbacks; the compositor owns
  refresh/VRR policy. Do not busy-wait for frame callbacks.
- **macOS**: deferred ([ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md), macOS via
  MoltenVK); use `CVDisplayLink`/`CAMetalDisplayLink` when MoltenVK lands.
- **Android**: deferred (see [`mobile-integration.md`](mobile-integration.md)); use `Choreographer` frame callbacks and display refresh rate APIs.

The loop must degrade gracefully when a platform cannot report refresh/VRR: assume 60 Hz,
`vsync=on`, no VRR, and record the assumption in diagnostics.

## 7. Multi-frame execution

- `gpuFramesInFlight` (1–3, default 2) bounds CPU/GPU overlap.
- Per-frame arenas: vertex/index/uniform data is written into a frame slot; never overwrite a
  slot still in flight.
- Timeline semaphores (Vulkan 1.3) gate slot reuse; fences gate one-shot uploads.
- Swapchain acquire/present is outside the simulation; the renderer records into the acquired
  image.
- Resize/display change waits for in-flight frames, recreates targets, and marks all paint/render
  caches dirty.
- The runtime never blocks on a GPU readback in the frame loop. Readback is for tests/tools only.

## 8. Deterministic mode

Deterministic mode (`runtime.deterministic=true`, forced for goldens) makes the loop a pure
function of inputs:

- simulated clock; no wall-clock reads
- fixed `fixedTimestepHz` and fixed step count
- fixed viewport/DPI/refresh (60 Hz logical)
- no vsync/sleep/present; render to an offscreen target
- recorded input script sampled at tick boundaries
- fixed animation interpolation or disabled interpolation
- stable iteration order in style/layout/paint

The runner API can be:

```cpp
for (int frame = 0; frame < frames; ++frame) {
    loop.tick_exact(1.0 / 60.0);
    capture(frame);
}
```

## 9. Threading

- V1 (first release; scope rule in [AGENTS.md Non-negotiables](../../AGENTS.md#non-negotiables) item 8):
  UI thread owns input sampling, style, layout, paint, and command recording.
- Platform input threads push into a single-producer/single-consumer queue; no locks in the
  steady-state path.
- A render thread is not part of V1 and has no decision yet (see section 12); the scene graph and style state remain UI-thread-owned.
- The frame pacer never blocks the UI thread on GPU, swapchain, fence, I/O, or cross-thread work
  ([AGENTS.md non-negotiable 12](../../AGENTS.md#non-negotiables), [`async.md`](async.md)).
  Between ticks the loop only idles by yielding to the OS event/timer wait of the platform loop
  (a timer for the next present deadline plus the edge-triggered wake sources of section 3.2:
  input, async completion posts, `request_frame()`), so input and completions are handled
  immediately and the loop never busy-waits. This idle wait is bounded by the next deadline and
  holds no locks. The HAL seam for the platform part is `wait_for_events_until(window, deadline)`
  from `hal/window_wait.hpp` (see [`hal.md`](hal.md) §1.2). `runtime::wait_for_next_frame(window, pacer)` (`runtime/frame_wait.hpp`) combines the FramePacer deadline with that wait; application loops call it between ticks (the windowed POC does, driving `RuntimeLoop::tick()` in continuous mode); `--on-demand` drives a scene through `RuntimeLoop` in render-on-demand mode, with scripted scene changes standing in for input. Acquire/present use the bounded state machine of [`async.md`](async.md). The app window driver (`WindowHost`, `src/app/window.cpp`) implements that policy through `app/idle_policy.hpp`: it turns `FramePacer::ready(now)` into the platform wait (never past the deadline, clamped to `kMaxIdleWaitSeconds` = 250 ms when no deadline is scheduled, so an uncapped settled scene still parks), does not begin a host frame at all while the loop says nothing is due, calls `AppHost::end_frame()` on a frame the host did not make due so dirt cannot stay latched, and presents only a frame whose damage/redraw plan has regions (forced `run(max_frames > 0)` runs are the bounded CI exception). Teardown polls only the lists the driver submitted, under a bounded budget, never a device-wide `wait_idle()`; `run()` may be called again (the input queue is reattached on entry).

## 10. Metrics and diagnostics

The runtime exposes:

- `frame_index`, `tick_index`, `sim_time`, `frame_dt`, `alpha`
- `effective_fps`, `present_mode`, `refresh_hz`, `vrr_active`
- `frame_time_ms`, `update_time_ms`, `style_time_ms`, `layout_time_ms`, `paint_time_ms`,
  `render_time_ms`, `present_wait_ms`
- `steps_this_frame`, `dropped_steps`, `missed_deadlines`
- `dirty_style_nodes`, `dirty_layout_nodes`, `dirty_paint_nodes`, `draw_calls`, `vertices`,
  `damage_area_ratio`
- `gpu_frames_in_flight`, `queued_frames`

The debug overlay shows a compact subset; the full set is available to tests and tools.

## 11. Tests

- Accumulator math: 60 Hz simulation under 30/60/144 Hz render cadence.
- `timeScale` 0 (pause), 0.5, 2.0; no drift over long runs.
- `maxFrameDelta` clamp and `maxUpdatesPerFrame` guard.
- Frame cap and refresh divisor selection.
- On-demand wake sources; idle does not spin.
- Deterministic mode: identical outputs for identical input scripts.
- VRR fallback: unsupported capabilities choose a valid present mode.
- Resize/display-change invalidation.

## 12. Open questions

- Exact VRR capability APIs per platform and driver quirks.
- Whether to interpolate layout positions or only paint transforms in V1 (first release).
- Render-thread architecture and command-buffer reuse.
- Power-save behavior when the window is occluded/minimized (section 4.4 says paused by default under
  `pauseWhenOccluded`; what `power-save` pacing adds beyond that is not specified).
