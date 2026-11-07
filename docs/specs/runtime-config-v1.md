# Runtime configuration v1

Status: proposed for Big Step B2/B3. Extends [`project-config-v1.md`](project-config-v1.md)
with the `runtime` object. The C++ API mirror is `tessera::RuntimeConfig`.

## 1. Goals

- Make frame pacing, FPS caps, vsync/VRR, dirty rendering, and deterministic mode explicit.
- Keep defaults safe and predictable on every platform.
- Separate **simulation rate** from **present rate**.
- Allow the same project to run continuous (dashboard/video) or on-demand (static tool UI).
- Never require the application to hand-roll a game loop.

## 2. Schema

```json
{
  "runtime": {
    "fixedTimestepHz": 60,
    "maxFps": 0,
    "vsync": "on",
    "dynamicRefreshRate": "auto",
    "preferredRefreshRate": 0,
    "framePacing": "auto",
    "latencyFrames": 2,
    "allowTearing": false,
    "maxFrameDeltaMs": 250,
    "maxUpdatesPerFrame": 5,
    "renderScale": 1.0,
    "renderOnDemand": true,
    "powerPolicy": "adaptive",
    "adaptiveQuality": true,
    "minRenderScale": 0.6,
    "frameBudgetMs": 0,
    "pauseWhenOccluded": true,
    "pauseWhenUnfocused": false,
    "backgroundMaxFps": 0,
    "resizeMode": "adaptive",
    "resizeDebounceMs": 16,
    "swapchainRecreateDelayMs": 16,
    "maxPendingResizes": 1,
    "asyncWorkerThreads": 0,
    "asyncIoThreads": 1,
    "asyncQueueCapacity": 4096,
    "asyncMainThreadBudgetMs": 2.0,
    "asyncTraceTasks": false,
    "dirtyRendering": "auto",
    "damageRects": true,
    "maxDamageRects": 16,
    "fullRedrawThreshold": 0.6,
    "layerCacheBudgetMb": 64,
    "drawListCacheBudgetMb": 32,
    "gpuFramesInFlight": 2,
    "animationInterpolation": true,
    "deterministic": false
  }
}
```

## 3. Fields

| Field | Type | Default | Range / values | Notes |
|---|---|---|---|---|
| `fixedTimestepHz` | number | `60` | `1..1000` | Simulation tick rate. Deterministic. |
| `maxFps` | integer | `0` | `0..1000` | Presented frames/sec cap. `0` = uncapped (display/vsync limits). |
| `vsync` | string | `on` | `off`, `on`, `adaptive`, `mailbox` | Present mode policy. |
| `dynamicRefreshRate` | string | `auto` | `off`, `auto`, `preferred` | VRR/refresh-rate policy. |
| `preferredRefreshRate` | integer | `0` | `0` or `24..1000` | Used when `dynamicRefreshRate=preferred`. `0` = platform default. |
| `framePacing` | string | `auto` | `auto`, `smooth`, `low-latency`, `power-save` | Pacing strategy. |
| `latencyFrames` | integer | `2` | `1..3` | Max queued frames; lower = lower latency. |
| `allowTearing` | boolean | `false` | | Required for `vsync=off` on platforms that gate tearing. |
| `maxFrameDeltaMs` | number | `250` | `1..5000` | Clamp for catch-up after a stall. |
| `maxUpdatesPerFrame` | integer | `5` | `1..16` | Spiral-of-death guard. |
| `renderScale` | number | `1.0` | `0.25..4.0` | Offscreen/swapchain resolution scale. |
| `renderOnDemand` | boolean | `true` | | Render only on dirty/wake events; static UI idles. |
| `powerPolicy` | string | `adaptive` | `adaptive`, `power-save`, `balanced`, `performance`, `benchmark` | Preset for power/quality behavior. |
| `adaptiveQuality` | boolean | `true` | | Reduce optional cost when frame budgets are missed. |
| `minRenderScale` | number | `0.6` | `0.25..1.0` | Lower bound for adaptive resolution scaling. |
| `frameBudgetMs` | number | `0` | `0..1000` | `0` = derive from effective rate; otherwise CPU+GPU target. |
| `pauseWhenOccluded` | boolean | `true` | | Stop rendering/animations when hidden/occluded. |
| `pauseWhenUnfocused` | boolean | `false` | | Stop or throttle when the window loses focus. |
| `backgroundMaxFps` | integer | `0` | `0..1000` | Cap when unfocused but visible; `0` = use `maxFps`. |
| `resizeMode` | string | `adaptive` | `live`, `deferred`, `adaptive` | Live vs deferred swapchain/target recreation. |
| `resizeDebounceMs` | number | `16` | `0..1000` | Coalesce resize events before layout/recreate. |
| `swapchainRecreateDelayMs` | number | `16` | `0..1000` | Delay after resize settles before recreation. |
| `maxPendingResizes` | integer | `1` | `1..16` | Coalescing queue bound; excess events merge. |
| `asyncWorkerThreads` | integer | `0` | `0..256` | `0` = auto (hardware concurrency minus UI/IO). |
| `asyncIoThreads` | integer | `1` | `1..16` | Dedicated blocking I/O threads. |
| `asyncQueueCapacity` | integer | `4096` | `64..1048576` | Bounded completion/task queue. |
| `asyncMainThreadBudgetMs` | number | `2.0` | `0..16` | Per-tick main-thread completion budget. |
| `asyncTraceTasks` | boolean | `false` | | Trace task spans/queue latency; debug overhead. |
| `dirtyRendering` | string | `auto` | `auto`, `on`, `off` | `auto` enables dirty mode when beneficial. |
| `damageRects` | boolean | `true` | | Partial redraw when the present path supports it. |
| `maxDamageRects` | integer | `16` | `1..64` | Exceeding this falls back to full redraw. |
| `fullRedrawThreshold` | number | `0.6` | `0.0..1.0` | Damage area ratio above which to full-redraw. |
| `layerCacheBudgetMb` | integer | `64` | `0..4096` | Retained layer cache budget. `0` disables layers. |
| `drawListCacheBudgetMb` | integer | `32` | `0..4096` | Draw-list cache budget. `0` disables caching. |
| `gpuFramesInFlight` | integer | `2` | `1..3` | CPU/GPU overlap; 1 = lowest memory, 3 = deepest pipeline. |
| `animationInterpolation` | boolean | `true` | | Interpolate visual state at render rate. |
| `deterministic` | boolean | `false` | | Simulated clock, fixed steps, no vsync/sleep. Forced in goldens. |

## 4. Validation rules

1. `fixedTimestepHz > 0`.
2. `maxFps = 0` means uncapped; a non-zero value is a hard present cap.
3. `vsync=off` requires `allowTearing=true` on platforms with tearing gates; otherwise the
   runtime coerces to `on` with a warning.
4. `dynamicRefreshRate=preferred` requires `preferredRefreshRate > 0`.
5. `preferredRefreshRate` must be `0` or in `24..1000`.
6. `latencyFrames`, `gpuFramesInFlight`, and `maxUpdatesPerFrame` must be within their ranges.
7. `renderScale` is clamped to `0.25..4.0`.
8. `fullRedrawThreshold` is a ratio; `1.0` means "always damage-redraw unless full".
9. `dirtyRendering=off` disables dirty caches for debugging; it does not change visual output.
10. `deterministic=true` forces a simulated clock and ignores `vsync`, `maxFps`, and
    `renderOnDemand` for the duration of the run.
11. Unknown fields are errors (strict schema).
12. `layerCacheBudgetMb=0` or `drawListCacheBudgetMb=0` is valid and disables that cache.
13. `minRenderScale <= renderScale`; `renderScale` may be reduced down to `minRenderScale` by
    adaptive quality.
14. `backgroundMaxFps=0` means "use `maxFps`"; it is not a pause. Use
    `pauseWhenUnfocused=true` or `pauseWhenOccluded=true` to stop rendering.
15. `powerPolicy=benchmark` is allowed but emits a diagnostic and must not be shipped.
16. `resizeMode` is `live`, `deferred`, or `adaptive`; debounce/recreate delays are
    `0..1000 ms`.
17. `maxPendingResizes` is `1..16`; excess resize events are coalesced, never queued unbounded.
18. `asyncWorkerThreads=0` means auto; `asyncIoThreads >= 1`.
19. `asyncQueueCapacity` is `64..1048576`; overflow applies backpressure/cancellation, never
    unbounded growth.
20. `asyncMainThreadBudgetMs` is `0..16`; completions beyond the budget defer to the next tick.

## 5. Semantics

### 5.1 Simulation vs present

- `fixedTimestepHz` controls simulation.
- `maxFps`, `vsync`, `dynamicRefreshRate`, and `framePacing` control presentation.
- `maxFps` never changes simulation behavior.
- `timeScale` is a runtime API (`Time::SetTimeScale`), not a config field; `0` pauses.

### 5.2 `maxFps` and vsync

Effective present rate:

```text
if maxFps == 0: rate = refresh (or VRR content rate)
else:           rate = min(maxFps, refresh)
```

When `vsync=on`, a cap below the refresh rate uses a refresh divisor if possible; otherwise the
pacer uses deadlines and reports jitter.

### 5.3 Dynamic refresh rate

- `off`: fixed/preferred refresh; no VRR negotiation.
- `auto`: use VRR when the platform/display supports it; otherwise preferred/desktop refresh.
- `preferred`: request `preferredRefreshRate` when supported; otherwise fall back to `auto`.

VRR never overrides `maxFps`; a non-zero cap still applies.

### 5.4 Dirty rendering

- `auto`: dirty style/layout/paint/render plus damage when safe.
- `on`: always use dirty paths (for profiling/validation).
- `off`: full rebuild/redraw every frame (debug/regression comparison).

`damageRects` is a separate switch. Even with `dirtyRendering=on`, the renderer may full-redraw
when damage is unsafe or above `fullRedrawThreshold`.

### 5.5 Deterministic mode

In deterministic mode:

- simulate a fixed step count; no wall clock;
- force offscreen rendering; ignore present modes;
- fix viewport, DPI, and refresh inputs;
- disable interpolation if the golden mode requires it;
- record/consume an input script.

### 5.6 Power policy

`powerPolicy` is a preset applied at load time; explicit fields in the same object override it.

| Policy | Present default | Dirty default | Adaptive | Notes |
|---|---|---|---|---|
| `adaptive` | on-demand, `maxFps=0` | on | on | normal apps; idle when static |
| `power-save` | on-demand, `maxFps=30` | on | aggressive | battery/kiosk |
| `balanced` | on-demand, `maxFps=60` | on | moderate | desktop apps |
| `performance` | continuous, `maxFps=0` | on | off | live dashboards/video |
| `benchmark` | uncapped, full redraw | off | off | diagnostics only |

- `adaptive` is the default and is the recommended policy for application UIs.
- `performance` may render continuously but still honors dirty work and explicit caps.
- `benchmark` must be reported in logs and is rejected for release packaging by tooling.
- A policy must never silently change simulation dt or correctness.
- Hardware utilization is controlled by work (dirty/damage/scale/effects/present rate), not by a
  "GPU utilization %" target. See [`performance.md`](../design/performance.md).

### 5.7 Adaptive quality

When `adaptiveQuality=true` and a frame budget is missed:

1. reduce optional effects (blur/large shadows/extra AA),
2. lower `renderScale` toward `minRenderScale`,
3. lower the present cap to the next refresh divisor,
4. pause non-essential animations,
5. report the downgrade.

Use hysteresis/cooldown; never oscillate every frame. Never skip correctness, dirty
invalidation, or input responsiveness. All changes are visible in metrics.

### 5.8 Idle, occluded, and background behavior

- **Static:** no presents, no GPU submissions, no animation ticks after settling.
- **Input:** wakes exactly the frame(s) needed, then idles again.
- **Occluded/minimized:** `pauseWhenOccluded=true` stops rendering and active animations;
  restoring marks the target dirty and renders one frame.
- **Unfocused but visible:** `pauseWhenUnfocused=true` stops rendering; otherwise
  `backgroundMaxFps` caps it (`0` = use `maxFps`).
- **VRR:** do not present empty frames to keep VRR alive; idle between real frames.

### 5.9 Resize and DPI

- `resizeMode` is infrastructure policy; styles/animations never branch on it.
- Resize events are coalesced to the latest size, consumed at a frame boundary, then
  style environment -> style -> layout -> paint -> target -> render.
- Keep the old swapchain/target until the new one is ready and in-flight frames retire.
- `resizeDebounceMs`/`swapchainRecreateDelayMs` tune live resize; they do not change layout
  semantics.
- Zero-size/minimized skips target creation; restore marks the root dirty and renders one frame.
- OS event mapping, display capabilities, and fold posture/hinge/occlusion are in
  [`os-events-v1.md`](os-events-v1.md).
- See [`window-resize.md`](../design/window-resize.md).

### 5.10 Async completions

- `asyncWorkerThreads=0` means auto; `asyncIoThreads` are for blocking I/O.
- Queues are bounded (`asyncQueueCapacity`); overflow applies backpressure/cancellation.
- Completions are drained at tick boundaries on the main thread with
  `asyncMainThreadBudgetMs`; excess defers to the next tick.
- No blocking UI waits: GPU fence/acquire/present and I/O use async events/state machines.
- `asyncTraceTasks` enables task spans/latency in diagnostics.
- See [`async.md`](../design/async.md).

## 6. C++ API mirror

```cpp
namespace tessera {

enum class VsyncMode { Off, On, Adaptive, Mailbox };
enum class RefreshMode { Off, Auto, Preferred };
enum class FramePacing { Auto, Smooth, LowLatency, PowerSave };
enum class DirtyMode { Auto, On, Off };
enum class PowerPolicy { Adaptive, PowerSave, Balanced, Performance, Benchmark };
enum class ResizeMode { Live, Deferred, Adaptive };

struct RuntimeConfig {
    double fixed_timestep_hz = 60.0;
    std::uint32_t max_fps = 0;
    VsyncMode vsync = VsyncMode::On;
    RefreshMode dynamic_refresh_rate = RefreshMode::Auto;
    std::uint32_t preferred_refresh_rate = 0;
    FramePacing frame_pacing = FramePacing::Auto;
    std::uint32_t latency_frames = 2;
    bool allow_tearing = false;
    double max_frame_delta_ms = 250.0;
    std::uint32_t max_updates_per_frame = 5;
    float render_scale = 1.0f;
    bool render_on_demand = true;
    PowerPolicy power_policy = PowerPolicy::Adaptive;
    bool adaptive_quality = true;
    float min_render_scale = 0.6f;
    double frame_budget_ms = 0.0;
    bool pause_when_occluded = true;
    bool pause_when_unfocused = false;
    std::uint32_t background_max_fps = 0;
    ResizeMode resize_mode = ResizeMode::Adaptive;
    double resize_debounce_ms = 16.0;
    double swapchain_recreate_delay_ms = 16.0;
    std::uint32_t max_pending_resizes = 1;
    std::uint32_t async_worker_threads = 0;
    std::uint32_t async_io_threads = 1;
    std::uint32_t async_queue_capacity = 4096;
    double async_main_thread_budget_ms = 2.0;
    bool async_trace_tasks = false;
    DirtyMode dirty_rendering = DirtyMode::Auto;
    bool damage_rects = true;
    std::uint32_t max_damage_rects = 16;
    float full_redraw_threshold = 0.6f;
    std::uint32_t layer_cache_budget_mb = 64;
    std::uint32_t draw_list_cache_budget_mb = 32;
    std::uint32_t gpu_frames_in_flight = 2;
    bool animation_interpolation = true;
    bool deterministic = false;
};

}
```

## 7. Platform support

| Capability | Windows | Linux/X11 | Linux/Wayland | Android | macOS |
|---|---|---|---|---|---|
| `maxFps` cap | yes (waitable swapchain / timer) | yes | yes (frame callbacks) | later | later |
| `vsync` modes | flip model | Present ext / driver | compositor | later | later |
| tearing | `ALLOW_TEARING` | driver/extension | compositor | later | later |
| dynamic refresh/VRR | OS/driver | driver/compositor | compositor | later | later |
| damage rects | DXGI dirty rects | Present damage | surface damage | later | later |
| resize/DPI/occlusion | `WM_SIZE`/`WM_DPICHANGED` | `ConfigureNotify`/RandR | `xdg configure`/`ack` | later | later |
| fold/posture | no standard hinge API | no standard hinge API | compositor-specific | Jetpack WindowManager | no mainstream foldable |
| async scheduler | portable C++20 | portable | portable | portable | portable |

Missing capability -> documented fallback, never a crash. The selected mode is reported in the
diagnostics/metrics.

## 8. Defaults rationale

- `fixedTimestepHz=60`: deterministic baseline for goldens and animations.
- `maxFps=0`: uncapped lets the platform/vsync decide; avoids fighting VRR.
- `vsync=on`: stable first-run behavior, no tearing.
- `dynamicRefreshRate=auto`: use VRR when available, fall back safely.
- `dirtyRendering=auto`: optimize by default, but correctness-first fallbacks.
- `gpuFramesInFlight=2`: enough overlap for smooth input latency without deep queues.
- `renderOnDemand=true`: static scenes idle; active animations/data streams are wake sources and
  still render every tick. Continuous video/benchmark modes opt out explicitly.
- `powerPolicy=adaptive`: on-demand + dirty + adaptive quality; the safe default for apps.
- `adaptiveQuality=true`: recover from frame-budget misses without application code.
- `pauseWhenOccluded=true`: no GPU work when the window is hidden/minimized.
- `backgroundMaxFps=0`: use `maxFps` when unfocused but visible; opt into throttling explicitly.
- `frameBudgetMs=0`: derive the budget from the effective present rate.
- `resizeMode=adaptive`: live for small changes, defer/throttle large or slow frames.
- `maxPendingResizes=1`: resize storms coalesce to the latest size, never an unbounded queue.
- `asyncWorkerThreads=0`: auto; `asyncIoThreads=1`; task/completion queues are bounded.
- `asyncMainThreadBudgetMs=2`: completion floods cannot stall input or rendering.
- `asyncTraceTasks=false`: task tracing is a diagnostic/debug cost, not a default.

## 9. Interaction with `project-config-v1.md`

- `window` defines initial size/DPI/title; `runtime` defines pacing/presentation.
- `dev` may add an overlay and hot-reload; it must not change runtime semantics.
- `build` may change `renderScale` only through presets, not silently.
- `deterministic` is forced by `tessera test`/golden runs regardless of the project file.

## 9.1 Examples

### 144 Hz VRR dashboard

```json
{
  "runtime": {
    "fixedTimestepHz": 60,
    "maxFps": 0,
    "vsync": "adaptive",
    "dynamicRefreshRate": "auto",
    "framePacing": "smooth",
    "latencyFrames": 2,
    "dirtyRendering": "auto",
    "damageRects": true,
    "renderOnDemand": false,
    "powerPolicy": "adaptive"
  }
}
```

Content presents when ready; VRR follows the content rate up to the display maximum. The
simulation still ticks at 60 Hz. `renderOnDemand=false` is explicit here because the dashboard is
continuous; omit it to use the adaptive/idle default.

### 60 Hz low-latency tool UI

```json
{
  "runtime": {
    "fixedTimestepHz": 60,
    "maxFps": 60,
    "vsync": "mailbox",
    "dynamicRefreshRate": "off",
    "framePacing": "low-latency",
    "latencyFrames": 1,
    "renderOnDemand": true,
    "dirtyRendering": "on",
    "damageRects": true,
    "powerPolicy": "adaptive",
    "adaptiveQuality": true
  }
}
```

Static UI renders only on input/dirty wake; pointer-driven feedback has the lowest queue depth.

### 120 Hz cap on a 144 Hz display

```json
{ "runtime": { "maxFps": 120, "vsync": "on", "dynamicRefreshRate": "off" } }
```

The pacer uses a deadline schedule; it does not busy-wait. If a refresh divisor is required for
stability, use `maxFps=72` on a 144 Hz display instead.

### Power-save kiosk

```json
{
  "runtime": {
    "powerPolicy": "power-save",
    "maxFps": 30,
    "renderOnDemand": true,
    "pauseWhenOccluded": true,
    "adaptiveQuality": true
  }
}
```

The explicit `maxFps` overrides the policy preset. Static pages idle; animations/input are
capped at 30 fps and adaptive quality protects the budget.

### Benchmark mode (diagnostics only)

```json
{
  "runtime": {
    "powerPolicy": "benchmark",
    "maxFps": 0,
    "dirtyRendering": "off",
    "damageRects": false,
    "adaptiveQuality": false
  }
}
```

This is a profiling mode. It must not ship; tooling rejects it for release packaging.

### Frame-cap outcomes

| Display | `maxFps` | `vsync` | Effective |
|---|---|---|---|
| 60 Hz | `0` | `on` | 60 |
| 144 Hz | `0` | `on` | 144 |
| 144 Hz | `72` | `on` | 72 (divisor) |
| 144 Hz | `120` | `on` | 120 (deadline) |
| 144 Hz VRR | `0` | `adaptive` | content rate, up to 144 |
| any | `0` | `off` + tearing | uncapped, may tear |
| static UI | `30` | any | 30 while active, idle when on-demand |

## 10. Metrics exposed

At minimum:

- `effective_fps`, `refresh_hz`, `vrr_active`, `present_mode`
- `frame_time_ms`, `update_time_ms`, `paint_time_ms`, `render_time_ms`, `present_wait_ms`
- `gpu_frame_time_ms` (when `diagnostics.gpuTimestamps=true`), `idle_time_ms`
- `steps_this_frame`, `dropped_steps`, `missed_deadlines`
- `power_policy`, `adaptive_scale`, `quality_level`
- `resize_mode`, `resize_sequence`, `pending_resizes`, `swapchain_generation`
- `display_count`, `display_id`, `display_refresh_hz`, `hdr_active`, `color_gamut`
- `fold_state`, `fold_orientation`, `fold_occlusion`, `hinge_bounds`
- `safe_area`, `work_area`, `multi_window`, `picture_in_picture`
- `async_tasks`, `async_queue_depth`, `async_main_thread_latency_ms`
- `dirty_style_nodes`, `dirty_layout_nodes`, `dirty_paint_nodes`
- `draw_calls`, `vertices`, `damage_area_ratio`, `full_redraw_reason`

## 11. Open questions

- Whether `renderScale` should be per-monitor.
- Default `maxFps` for battery-powered devices (possibly a platform profile).
- VRR min/max refresh querying and content-rate hints.
- Power-save policy when the window is occluded/minimized.
- Multi-window refresh/VRR independence.
