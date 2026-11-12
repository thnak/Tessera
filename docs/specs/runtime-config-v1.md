# Runtime configuration v1

Status: proposed for Big Steps B2 and B3 (defined in [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step):
B2 = runtime slice, B3 = window/swapchain/OS path). Decisions behind these fields:
[ADR-017](../../decisions/ADR-017-input-and-fixed-step-runtime.md) (fixed-step runtime),
[ADR-018](../../decisions/ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering, frame pacing),
[ADR-019](../../decisions/ADR-019-power-aware-runtime.md) (power-aware defaults),
[ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md) (resize/lifecycle), and
[ADR-021](../../decisions/ADR-021-async-runtime.md) (async runtime). Extends [`project-config-v1.md`](project-config-v1.md)
with the `runtime` object. The C++ API mirror is `tessera::RuntimeConfig`. Buffer capacities and
user tunables are in [`buffer-config-v1.md`](buffer-config-v1.md). Startup/loading/auth budgets
and resource policy are in [`startup-and-loading.md`](../design/startup-and-loading.md) and
[`resource-lifecycle-v1.md`](resource-lifecycle-v1.md).

Terms used below: vsync = present synchronized to display refresh; VRR = variable refresh rate
(adaptive sync); swapchain = the set of images presented to a window; tick = one fixed-timestep
simulation step; damage = the screen region that changed and needs repainting; golden = a
deterministic reference image (see [`golden-tests-v1.md`](golden-tests-v1.md)); dirty domains =
style/layout/paint/render invalidation scopes (see the [glossary](../reference/glossary.md)).

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
    "layoutObservationMaxRounds": 4,
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
    "viewTransitionMaxMb": 64,
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
| `layoutObservationMaxRounds` | integer | `4` | `1..16` | Same-tick settle rounds for size observations; `1` delivers on the next tick ([`layout-observation.md`](../design/layout-observation.md) section 5.3). Out-of-range values clamp. |
| `asyncWorkerThreads` | integer | `0` | `0..256` | `0` = auto (hardware concurrency minus UI/IO). |
| `asyncIoThreads` | integer | `1` | `1..16` | Dedicated blocking I/O threads. |
| `asyncQueueCapacity` | integer | `4096` | `64..1048576` | Bounded completion/task queue. |
| `asyncMainThreadBudgetMs` | number | `2.0` | `0..16` | Per-tick main-thread completion budget. |
| `asyncTraceTasks` | boolean | `false` | | Trace task spans/queue latency; debug overhead. |
| `dirtyRendering` | string | `auto` | `auto`, `on`, `off` | `auto` enables dirty paths with the safety fallbacks of [`dirty-rendering.md`](../design/dirty-rendering.md) section 6.2; no other criterion is specified. |
| `damageRects` | boolean | `true` | | Partial redraw when the present path supports it. |
| `maxDamageRects` | integer | `16` | `1..64` | Exceeding this falls back to full redraw. |
| `fullRedrawThreshold` | number | `0.6` | `0.0..1.0` | Damage area ratio above which to full-redraw. |
| `layerCacheBudgetMb` | integer | `64` | `0..4096` | Retained layer cache budget. `0` disables layers. |
| `drawListCacheBudgetMb` | integer | `32` | `0..4096` | Draw-list cache budget. `0` disables caching. |
| `viewTransitionMaxMb` | integer | `64` | `0..4096` | Memory limit in MiB for the two window-sized textures of a view transition ([`view-transitions-v1.md`](view-transitions-v1.md) section 9). A transition that would exceed it is skipped, not an error. `0` skips every transition. |
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
21. `viewTransitionMaxMb` is `0..4096`. The size checked is 2 x width x height x 4 bytes of the window in pixels.

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

When `vsync=on`, a cap below the refresh rate uses a refresh divisor when one exists (the cap equals
the refresh rate divided by an integer); otherwise the pacer uses deadlines and reports jitter.

### 5.3 Dynamic refresh rate

- `off`: fixed/preferred refresh; no VRR negotiation.
- `auto`: use VRR when the platform/display supports it; otherwise preferred/desktop refresh.
- `preferred`: request `preferredRefreshRate` when supported; otherwise fall back to `auto`.

VRR never overrides `maxFps`; a non-zero cap still applies.

### 5.4 Dirty rendering

- `auto`: dirty style/layout/paint/render plus damage when partial redraw is safe, as defined in
  [`dirty-rendering.md`](../design/dirty-rendering.md) section 6.2.
- `on`: always use dirty paths (for profiling/validation).
- `off`: full rebuild/redraw every frame (debug/regression comparison).

`damageRects` is a separate switch. Even with `dirtyRendering=on`, the renderer may full-redraw
when damage is unsafe or above `fullRedrawThreshold`.

### 5.5 Deterministic mode

In deterministic mode:

- simulate a fixed step count; no wall clock;
- force offscreen rendering; ignore present modes;
- fix viewport, DPI, and refresh inputs;
- disable interpolation if the golden mode requires it (which golden modes require it is not
  specified here; see [`golden-tests-v1.md`](golden-tests-v1.md));
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
    std::uint32_t view_transition_max_mb = 64;
    std::uint32_t gpu_frames_in_flight = 2;
    bool animation_interpolation = true;
    bool deterministic = false;
};

}
```

### 6.1 Loader

[`include/tessera/runtime/config.hpp`](../../include/tessera/runtime/config.hpp) and
[`src/runtime/config.cpp`](../../src/runtime/config.cpp) implement the mirror above. The loader
takes the **whole** `tessera.json` text and reads its optional `runtime` member; an absent member,
`"runtime": null` and `{}` all yield the section 3 defaults. Only a `std::string_view` overload is
exposed (no harness JSON type): `tessera_runtime` sits below the harness in the link graph.

```cpp
namespace tessera {

enum class RuntimeConfigErrorCode {
    MalformedJson, // not JSON, duplicate key, invalid UTF-8/escape, nesting too deep
    InvalidType,   // wrong JSON type, including an integer field written as `60.0` or `1e2`
    InvalidValue,  // enum string outside the documented set (message lists the set)
    OutOfRange,    // outside a range the table does not clamp
    NonFinite,     // NaN/+-inf, including a literal that overflows double (`1e400`)
    CrossField,    // minRenderScale > renderScale
    UnknownField,  // a key that is not a runtime-config field (section 4 rule 11)
};

struct RuntimeConfigError {
    RuntimeConfigErrorCode code;
    std::string field;   // JSON key; empty for a document-level error
    std::string message; // names the field, the allowed enum values, or both cross-field keys
    std::size_t offset;  // byte offset of the offending value or syntax error
};

std::expected<RuntimeConfig, RuntimeConfigError>
parse_runtime_config(std::string_view project_json);

}
```

Rules applied by the loader:

- **Clamp vs error.** Exactly two fields clamp, matching the table's own wording: `renderScale`
  (`0.25..4.0`, section 4 rule 7) and `layoutObservationMaxRounds` (`1..16`, "Out-of-range values
  clamp"). Every other range is an error: where the table gives no clamp rule, the loader rejects
  rather than clamps. `preferredRefreshRate` accepts `0` or `24..1000` and rejects `1..23`.
- **Integer strictness.** An `integer` field needs a JSON integer literal; `60.0` and `1e2` are
  `InvalidType`, because the table types them as integers.
- **Cross-field.** `minRenderScale <= renderScale` is checked after clamping, so
  `renderScale=10, minRenderScale=1` is valid (10 clamps to 4) while an inverted pair is a
  `CrossField` error naming both keys. Consequence: lowering `renderScale` below the default
  `minRenderScale=0.6` without also lowering `minRenderScale` is an error, as rule 13 requires.
- **Non-finite.** A number that is not finite (`1e400`, `-1e400`) is `NonFinite`. JSON has no
  `NaN`/`inf` spelling, so those tokens are `MalformedJson`.
- **Unknown keys** in the `runtime` object are an `UnknownField` error (section 4 rule 11: strict
  schema), naming the first unknown key in document order. There is no tolerant mode: a project
  file written for a newer runtime must be read by that newer runtime, and a typo must not be
  silently ignored.
- **Determinism.** The first error in table order wins (unknown keys are checked first, in
  document order), no clock/RNG/locale is consulted, and the returned `RuntimeConfig` owns no
  allocation. The loader
  prints nothing; diagnostics are values.

Section 6's code block above omits `layoutObservationMaxRounds`, which section 3 and
[`layout-observation.md`](../design/layout-observation.md) section 5.3 define; the header includes
it with default `4`. This is model-only unless the key appears in section 6.2.

### 6.2 Consumed keys (wiring status)

The loader lands first; consumers wire one key at a time. Exactly two keys reach runtime code today,
through the pure mapping layer `src/app/window_config.hpp`:

| Key | Consumer | Effect |
|---|---|---|
| `animationInterpolation` | `FramePipelineConfig::interpolation` | `false`: the pipeline never creates an `InterpolationPaintView`, so paint uses the authoritative scene/style state instead of blending the previous and current tick samples. Default `true`. |
| `pauseWhenOccluded` | `WindowPausePolicy` (`pause_when_occluded`, `pause_when_minimized`) | `false`: an occluded or minimized window keeps rendering; `true` (default) pauses both. Focus/blur is never part of the policy. |

`WindowOptions::runtime` is the single entry point for those keys, and its default matches
`RuntimeConfig{}`, so every existing caller keeps today's behaviour. Every other key is still
unwired: `fixedTimestepHz`, `maxFps`, `vsync`, `dynamicRefreshRate`, `preferredRefreshRate`,
`framePacing`, `latencyFrames`, `allowTearing`, `maxFrameDeltaMs`, `maxUpdatesPerFrame`,
`renderScale`, `renderOnDemand`, `powerPolicy`, `adaptiveQuality`, `minRenderScale`,
`frameBudgetMs`, `pauseWhenUnfocused`, `backgroundMaxFps`, `resizeMode`, `resizeDebounceMs`,
`swapchainRecreateDelayMs`, `maxPendingResizes`, `layoutObservationMaxRounds`, the `async*` keys,
the dirty/damage/cache keys, `gpuFramesInFlight` and `deterministic`. Setting one of them changes
nothing yet; the driver keeps its built-in behaviour
([`window-resize.md`](../design/window-resize.md) section 8.2).

## 7. Platform support

| Capability | Windows | Linux/X11 | Linux/Wayland | Android | macOS |
|---|---|---|---|---|---|
| `maxFps` cap | yes (waitable swapchain / timer) | yes | yes (frame callbacks) | not specified | not specified |
| `vsync` modes | flip model | Present ext / driver | compositor | not specified | not specified |
| tearing | `ALLOW_TEARING` | driver/extension | compositor | not specified | not specified |
| dynamic refresh/VRR | OS/driver | driver/compositor | compositor | not specified | not specified |
| damage rects | DXGI dirty rects | Present damage | surface damage | not specified | not specified |
| resize/DPI/occlusion | `WM_SIZE`/`WM_DPICHANGED` | `ConfigureNotify`/RandR | `xdg configure`/`ack` | not specified | not specified |
| fold/posture | no standard hinge API | no standard hinge API | compositor-specific | Jetpack WindowManager | no mainstream foldable |
| async scheduler | portable C++23 | portable | portable | portable | portable |

"not specified" in the Android and macOS columns means no behavior has been decided yet (Android
is in Big Step F and macOS follows [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md);
both in [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step)). The Adaptive column of
section 5.6 (`moderate`, `aggressive`) is likewise not quantified in any spec.

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
