# Window resize, DPI, and lifecycle

Status: design for Big Step B3 (window/swapchain path; B3 and the other Big Steps are defined in
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision record:
[ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md) (window resize and lifecycle).
This document draws the line between
**declarative style/animation** and **window/renderer infrastructure**. HAL = hardware/OS abstraction
layer ([`hal.md`](hal.md)); tick, golden, VRR and `WindowMetrics` are in
[`glossary.md`](../reference/glossary.md).

## 0. Implemented subset (B3 window/swapchain slice)

The B3 seams now have real Windows and Linux/X11 implementations:

- [`include/tessera/hal/window.hpp`](../../include/tessera/hal/window.hpp) defines `WindowInsets`,
  `WindowMetrics`, `WindowEventType`/`WindowEvent`, `WindowDesc` (logical client size), the
  abstract `hal::Window` interface (`metrics`, `poll_events`, `show`, `hide`, `request_redraw`,
  `close`), and `hal::create_window`. The header still carries no OS handle: the native handle is
  private to the platform source.
- `src/platform/windows/window.cpp` implements the Win32 window with per-monitor DPI awareness
  v2, `WM_SIZE`/`WM_DPICHANGED`/`WM_SETFOCUS`/`WM_KILLFOCUS`/`WM_CLOSE`/`WM_SHOWWINDOW`
  normalization, a bounded (fixed-capacity, resize-coalescing) event queue, client-rect + DPI
  `WindowMetrics`, show/hide, `InvalidateRect` redraw requests, and owner-thread destruction.
- `src/platform/linux/window.cpp` implements the X11 window with Xlib: `XOpenDisplay`, a hidden
  `XCreateWindow`, `WM_PROTOCOLS`/`WM_DELETE_WINDOW` close, `ConfigureNotify` -> `Resize`,
  `FocusIn`/`FocusOut` -> `Focus`/`Blur`, `Expose` -> coalesced redraw wake, `ClientMessage`
  `WM_DELETE_WINDOW` -> `Close`, `XMapWindow`/`XUnmapWindow` show/hide, `XClearArea` redraw
  requests, and client-rect metrics whose DPI comes from the `Xft.dpi` Xresource (96 fallback,
  clamped to 48-768). CMake probes `find_package(X11 QUIET)` on Linux; without X11 the
  `src/hal/window/window_stub.cpp` path is built and `create_window` returns `nullptr`.
  `src/hal/window/window_event_queue.hpp` is the private shared ring (resize coalescing).
- [`include/tessera/rendering/rhi/swapchain.hpp`](../../include/tessera/rendering/rhi/swapchain.hpp)
  defines `SwapchainDesc` with opaque `native_window`/`native_display` handles supplied by the
  HAL, `SwapchainError` (including `Suboptimal`), the `Swapchain` interface, and
  `Device::create_swapchain`. `Swapchain::image_target(index)` exposes the acquired image as a
  presentable `RenderTarget`; the renderer's end-of-pass transitions it to `PRESENT_SRC_KHR` (not
  a transfer layout) and readback rejects it. The null backend returns a deterministic rotating
  swapchain (resize -> one `OutOfDate`, forced-timeout test control, stats).
- `src/rendering/backend/vulkan/src/vulkan_swapchain.cpp` and
  `vulkan_swapchain_present.cpp` share one surface/capability/format/present-mode selection core.
  The Win32 path uses `vkCreateWin32SurfaceKHR`; the X11 path uses `vkCreateXlibSurfaceKHR` with
  `native_display` (`Display*`) and `native_window` (`Window`) and queries
  `vkGetPhysicalDeviceXlibPresentationSupportKHR` for queue selection. Both create image views,
  acquire with a bounded fence, present, and retire the old swapchain (`oldSwapchain` plus a
  bounded empty-submit drain **before** destroying the retired images/views). Both swapchain
  smoke tests now render a real frame through `Batched2DRenderer` into the acquired image and
  present it, exercising the image-target bridge rather than presenting an empty image. Both
  also run a 12-frame `submit_async` loop with two frames in flight (present queued right behind
  the submit, waiting through the image's semaphore) under validation, which fails on a
  `[vulkan]` hazard message. Missing
  handles, unsupported surface formats, a surface-incapable queue,
  and builds without either platform surface extension report `NotSupported`.
- Event-driven idling exists at the HAL seam: `hal::Window::wait_for_events` (Win32
  `MsgWaitForMultipleObjectsEx` message pump; X11 `poll()` on the connection fd followed by an
  `XPending` drain) and
  [`include/tessera/hal/window_wait.hpp`](../../include/tessera/hal/window_wait.hpp)'s
  `wait_for_events_until(window, deadline_seconds)`. `runtime::wait_for_next_frame` combines
  that wait with the `FramePacer` deadline. The `batched2d_poc --windowed --frames N` path is the
  end-to-end demo: hidden platform window -> `Swapchain::image_target` render -> present ->
  pacer/event wait, with resize/OutOfDate recovery; it is registered as
  `batched2d_windowed_smoke` on Windows and under Xvfb on Linux. The loop is driven by
  `RuntimeLoop` on `runtime::SteadyClock` (continuous mode: the demo has no scene to go dirty)
  and submits each frame with `submit_async` over a two-slot ring. Still pending: Wayland,
  real-GPU-reset validation of device loss (the minimal D7 recovery path in
  [§7](#7-device-loss-d7) and [`gpu/backends.md`](gpu/backends.md#81-device-loss-d7) is
  implemented, and the app window driver now recovers within a bounded budget instead of ending the
  run - [§7.2](#72-app-window-recovery-windowhost)), a render-on-demand loop fed by a real scene,
  and full OS compliance beyond the messages handled here. The platform rules in §4 describe the
  target behavior.

Known Linux/X11 limitations of this slice (the Mac/Windows rules in §4 describe the target):

- **X11 only.** Wayland, xdg-shell/`ack_configure`, fractional scale, and compositor-driven
  resize are not implemented.
- **DPI is a heuristic.** `Xft.dpi` is read once at creation and applies to every window/screen;
  RandR per-monitor scale, `_NET_WM_SYNC_REQUEST`, refresh/VRR, and display hotplug are not
  queried. A bare Xvfb display has no `Xft.dpi` and reports scale 1.
- **No occlusion/minimize.** `WindowMetrics.occluded`/`minimized` stay `false` and focus is
  tracked from `FocusIn`/`FocusOut` only (text input uses an XIM input context, see
  [`hal.md`](hal.md)). No window manager
  is required or spoken to beyond `WM_DELETE_WINDOW`.
- **No frames in flight.** Acquire/present are bounded (100 ms clamp) and presentation is
  drained at recreation, but there is no per-image fence/semaphore set and no render-target
  wrapper yet; images must be fully rendered before `present`.
- **CI is Xvfb + lavapipe.** Linux tests run under `xvfb-run` with the lavapipe ICD; the overlay
  and text goldens are bit-exact against a specific Mesa/LLVM build and can drift by a few
  pixels on a newer software driver (the same drift exists at the base commit).

## 1. Separation of concerns

### Declarative layer (what developers write)

- Styles: flex/grid, percentages, `min/max`, `aspect-ratio`, `clamp()`, viewport units
  (`vw/vh/vmin/vmax`), media queries, container queries.
- Animations: transitions/keyframes, durations/easing, state-driven animation.
- Components: layout constraints, content, optional size-observation callbacks.
- No window handles, swapchain calls, OS messages, DPI APIs, or presentation modes.

### Infrastructure layer (runtime/HAL owns)

- Platform window messages and lifecycle states.
- Logical/physical size, DPI scale, safe areas, display refresh/VRR, focus/occlusion.
- Event coalescing/debouncing and frame-boundary ordering.
- Style environment updates (viewport/media/container inputs).
- Layout invalidation, paint/render invalidation.
- Swapchain/render-target recreation, old-resource retirement, present-mode re-query.
- Frame pacing suspension/resume during live resize.
- Error handling: out-of-date/suboptimal swapchain, device loss, OOM, zero-size.

| Concern | Defined by (declarative) | Executed by (infrastructure) |
|---|---|---|
| Breakpoint/media query | developer CSS | runtime evaluates fixed environment inputs |
| `width:100%`, flex/grid, min/max | developer CSS | layout engine |
| Transition/keyframe on size | developer CSS/animation | animation runtime retargets values |
| Safe-area padding | theme/style tokens | HAL supplies `WindowMetrics.safe_area` |
| Window min/max constraints | app config + layout min/max | HAL applies OS hints |
| `WM_SIZE`/`ConfigureNotify`/`xdg configure` | — | HAL normalizes/coalesces |
| Swapchain/target recreation | — | renderer/runtime state machine |
| DPI scale / refresh / VRR | — | HAL/runtime re-queries and publishes |

**Contract:** the declarative layer receives immutable `WindowMetrics` (through the style
environment and layout constraints) and never sees an OS event or swapchain object.

```cpp
struct WindowMetrics {
    std::uint32_t logical_width = 0;
    std::uint32_t logical_height = 0;
    std::uint32_t physical_width = 0;
    std::uint32_t physical_height = 0;
    float dpi_scale = 1.0f;
    float refresh_hz = 60.0f;
    Rect safe_area;                 // logical pixels
    Rect work_area;
    Orientation orientation = Orientation::Landscape;
    bool focused = true;
    bool occluded = false;
    bool minimized = false;
    bool fullscreen = false;
    bool multi_window = false;
    bool picture_in_picture = false;
    DisplayId display = 0;
    FoldInfo fold;                  // hinge/fold, if any
};
```

The full OS event taxonomy and per-platform mapping (Windows/X11/Wayland/Android/iOS/macOS)
is in [`os-events-v1.md`](../specs/os-events-v1.md). `FoldInfo` and fold semantics are defined
there.

## 2. Resize pipeline

```text
OS event (WM_SIZE / ConfigureNotify / xdg configure)
  -> normalized WindowResizeEvent { metrics, reason, sequence }
  -> coalesce: keep first/last, accumulate reasons, drop stale sequences
  -> update style environment (viewport, DPI, media/container inputs)
  -> mark style dirty (media/container/viewport rules), layout dirty root
  -> retire/recreate swapchain or render target if needed
  -> layout -> paint -> render -> present
```

Rules:

1. **Frame boundary.** Resize is consumed at a tick/frame boundary (a tick is one fixed-step update, see [`game-loop.md`](game-loop.md)), not in the OS callback.
   The callback only records the event.
2. **Coalesce.** A live drag can produce hundreds of events; keep the latest size and the union
   of reasons. Render at most one resize frame per tick.
3. **Order.** Style environment -> style -> layout -> paint -> render target -> render.
   Never lay out against the old viewport and paint into the new target.
4. **No re-entrancy.** Do not run user code or block inside the platform callback.
5. **Old swapchain.** Keep the old swapchain alive until the new one is created and in-flight
   frames using it are retired. Never destroy in-use resources.
6. **Zero-size/minimized.** Skip target creation and rendering; keep the scene tree intact.
   Restore marks the root dirty and renders one frame.
7. **Failure.** Out-of-date/suboptimal -> recreate; device loss -> full device/resource rebuild;
   OOM -> reduce scale/quality and report; never crash in the resize path.

### 2.1 Live resize vs deferred resize

| Mode | Behavior | Use |
|---|---|---|
| `live` | Present at the new size as often as the compositor allows | desktop apps, interactive tools |
| `deferred` | Use the last stable size until resize settles, then recreate | expensive scenes, remote/virtual displays |
| `adaptive` | Live for small changes, defer/throttle for large or slow frames (the thresholds are not specified) | default (see [`runtime-config-v1.md`](../specs/runtime-config-v1.md)) |

`resizeMode` is infrastructure config; styles/animations never branch on it. Debouncing and
swapchain-recreate delay are runtime knobs, not component code.

### 2.2 Animation during resize

- A resize changes layout/constraints, not animation definitions.
- Transitions retarget to the new computed value; do not restart from the beginning on every
  resize event.
- Coalesce resize events before advancing animations; use the tick clock, not event timestamps.
- Reduced motion (`prefers-reduced-motion`) suppresses non-essential resize animation.
- Animation must not drive the window size (no layout feedback loops); layout is authoritative.

### 2.3 Render scale and text rasterization

Text rasterizes at the window's device scale, so a DPI change re-rasterizes glyphs and never
reuses bitmaps produced for another scale.

- **Quantization.** `WindowMetrics::dpi_scale` snaps onto the decided ladder
  `1.0 / 1.25 / 1.5 / 2.0` ([`ui/scale/render_scale.hpp`](../../include/tessera/ui/scale/render_scale.hpp)):
  nearest step, ties rounded up, clamped to `[1.0, 2.0]`. `dpi / 96` is the device scale, so
  96/120/144/192 dpi are exactly 1.0/1.25/1.5/2.0, while 108, 132 and 168 dpi are exact ties that
  round up to 1.25, 1.5 and 2.0. The ladder is not a uniform lattice above 1.5: there is no 1.75
  step, so 1.75 rounds up to 2.0. A non-finite or non-positive scale is rejected by the checked
  helpers (`InvalidDeviceScale`) and mapped to the ladder floor by the total one.
- **State.** One [`WindowRenderScale`](../../include/tessera/ui/scale/window_render_scale.hpp) per
  window is derived from `WindowMetrics` and recomputed only when the reported `dpi_scale` changes
  or the platform reports a monitor change (`notify_display_change`, because `WindowMetrics`
  carries no display id). A repeated snapshot is a flag return - no quantization and no revision
  bump - so the runtime may observe on every tick. `revision` advances only when the quantized step
  changes, so 1.20 -> 1.24 does not invalidate raster work, and a rejected snapshot leaves the
  live state untouched.
- **Atlases.** [`ScaleGlyphAtlas`](../../include/tessera/ui/text/scale_glyph_atlas.hpp) keeps one
  `GlyphAtlas` per ladder step, so an atlas built for 1.25 never serves 2.0. A DPI change releases
  the previous step only; the renderer's own step-change policy is in [`text.md`](text.md) §5. Page
  LRU and page generations stay per atlas, so eviction on one step cannot invalidate another.
- **Bound.** The set holds at most `kRenderScaleStepCount` (4) atlases; a request for an
  in-between or out-of-range scale quantizes onto an existing slot instead of creating one. With
  the default 512x512 RGBA pages and four pages per atlas the ladder costs at most 16 MiB, and a
  monitor change never allocates a fifth atlas.
- **Goldens.** Golden mode pins the scale explicitly ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md)),
  so the same scene at the same ladder step produces the same glyph placement. The DPI/scale AA
  matrix is in [`antialiasing.md`](antialiasing.md) §7.

`renderScale` in [`runtime-config-v1.md`](../specs/runtime-config-v1.md) §5.7 is a different knob:
the adaptive resolution multiplier owned by adaptive quality. Text rasterization keys on the
monitor-derived ladder step above, not on `renderScale`.

## 3. Declarative style and layout semantics

- **Viewport units** (`vw/vh/vmin/vmax`) resolve against the current logical viewport.
- **Media queries** re-evaluate when width/height/orientation/resolution/color-scheme/
  reduced-motion inputs change. Stable order; no wall clock.
- **Container queries** re-evaluate only when the container's content box changes.
- **Percentages/flex/grid** reflow; intrinsic text measurement updates.
- **Safe areas/notches** are layout inputs, not hardcoded paddings.
- **Min/max window size** comes from OS constraints and app config; components can express
  layout min/max but do not call OS APIs.
- **Resize observation** for components (if exposed) is delivered after layout on the UI thread,
  with a generation counter; it is not the raw OS resize event. Proposed design:
  [`layout-observation.md`](layout-observation.md).
- **DPI/scale** changes recompute logical sizes, media queries, hit-test coordinates, cursor
  assets, glyph rasterization, and render-target sizes.

## 4. OS compliance

### Windows

- Handle `WM_SIZE`, `WM_GETMINMAXINFO`, `WM_WINDOWPOSCHANGED`, `WM_DISPLAYCHANGE`,
  `WM_DPICHANGED`, `WM_SETTINGCHANGE`, `WM_THEMECHANGED`, `WM_ACTIVATEAPP`,
  `WM_SHOWWINDOW`, `WM_POWERBROADCAST`.
- Gate swapchain recreation with `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE`: coalesce during the modal
  resize loop, recreate on exit (or at a frame boundary with no frame in flight on the old swapchain), not once per `WM_SIZE`.
- Per-monitor DPI awareness v2 (`SetProcessDpiAwarenessContext`); use
  `AdjustWindowRectExForDpi` for non-client sizing; honor the suggested rect in `WM_DPICHANGED`.
- Honor system minimum/maximum track sizes; do not fight the shell.
- Use a flip-model swapchain/waitable object for pacing; handle `DXGI_ERROR_DEVICE_REMOVED`,
  `DXGI_ERROR_DEVICE_RESET`, `DXGI_ERROR_WAS_STILL_DRAWING`, `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`.
- Do not block the message loop with synchronous GPU/device waits. `DefWindowProc` must remain
  responsive; never `SendMessage` across threads in the resize path.

### Linux / X11

- Handle `ConfigureNotify`, RandR `RRScreenChangeNotify` (then `XRRUpdateConfiguration`),
  `PropertyNotify`, `_NET_WM_STATE`, and `WM_DELETE_WINDOW`.
- Use `_NET_WM_SYNC_REQUEST`/`_NET_WM_SYNC_REQUEST_COUNTER` when the compositor supports it to
  avoid tearing during resize; do not block waiting for the counter.
- Respect ICCCM size hints (`WM_NORMAL_HINTS`) and EWMH states (maximized, fullscreen, tiled).
- Re-query refresh/scale on RandR changes; use the Present extension for timing/damage.
- Keep the event loop non-blocking; `XSync`/round-trips must not stall the UI thread.

### Linux / Wayland

- Resize is compositor-driven: `xdg_surface.configure` / `xdg_toplevel.configure` provides the
  size and states. **Never draw before `ack_configure` for the serial you use.**
- Acknowledge every configure; use the last acknowledged size for layout/targets; discard stale
  buffers. Handle `configure_bounds`, `wm_capabilities`, maximized/fullscreen/tiled/activated.
- Use `wl_output.scale` and `wp_fractional_scale_v1`; apply `wp_viewporter` for fractional
  scaling; `wl_surface.set_buffer_scale` for integer scale.
- Pace with `wl_surface.frame` callbacks and `wp_presentation`; never busy-wait.
- Surface damage (`wl_surface.damage_buffer`) for partial updates; the compositor owns damage.
- Decorations: prefer server-side decorations when the compositor offers them; client-side decorations must
  respect `xdg_toplevel` states and hit-testing.
- Minimized/occluded: stop presenting; do not render empty frames.

### macOS / Android

- macOS (deferred; [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md), macOS via MoltenVK): `NSWindowDelegate` resize, `viewDidChangeBackingProperties` for scale,
  `CVDisplayLink`/`CAMetalDisplayLink` for pacing; MoltenVK surface recreation.
- Android (deferred to Big Step F, see [`mobile-integration.md`](mobile-integration.md)): `onSurfaceChanged`, `onConfigurationChanged`, `Choreographer` frame
  callbacks, multi-window/split-screen lifecycle.

### Cross-platform rules

- Respect safe areas, system DPI/scale, min/max sizes, title-bar/non-client behavior, system
  theme/high-contrast, reduced motion, and accessibility settings.
- Chrome appearance and first-frame timing are specified in
  [`window-chrome-v1.md`](../specs/window-chrome-v1.md).
- Multi-monitor: per-monitor DPI/scale/refresh; re-query present modes and VRR on display change.
- Do not assume 60 Hz or 1.0 scale when the platform cannot report them; use documented fallbacks.
- Never block the UI/message loop on the compositor, GPU, or a synchronous cross-thread call.
- Resize/DPI/theme/display changes are deterministic inputs; golden tests (deterministic reference images, [`golden-tests-v1.md`](../specs/golden-tests-v1.md)) synthesize them.

### 4.1 Mobile and foldables

- Mobile: orientation, safe areas/cutouts, IME insets, multi-window, and PiP are normalized
  separately from a desktop resize. See [`os-events-v1.md`](../specs/os-events-v1.md).
- Foldables: treat the fold as a **display feature** (`FoldInfo`) with state, orientation,
  occlusion, separating, and hinge bounds. It is not a separate UI mode.
- `HALF_OPENED` + horizontal fold -> tabletop layout; `HALF_OPENED` + vertical fold -> book
  layout; `occlusion=FULL`/`separating=true` means interactive content must avoid the hinge.
- Fold/unfold may stop and restart the app on Android; preserve/restore state for continuity.
- There is no stable cross-vendor hinge API on Windows/Linux; fall back to display topology,
  DPI, orientation, and size changes.
- Android/iOS surface, IME, accessibility, and manifest/Info.plist requirements are in
  [`mobile-integration.md`](mobile-integration.md).

## 5. Tests

- Synthetic resize sequences: grow/shrink, zero-size, minimize/restore, maximize/fullscreen,
  tiled, DPI change, multi-monitor move.
- Swapchain seam: null-backend descriptor validation, image rotation, out-of-date resize,
  configurable timeout, and missing/duplicate/out-of-range present rejection.
- Device loss: injected loss from submit/fence-wait/acquire, generation invalidation, one-shot
  recovery, rendering after recreation, and the failure path (see §7). The window driver adds a
  deterministic attempt-budget policy test and a real hidden-window GPU test (§7.2).
- Live-resize storms: coalescing, one resize frame per tick, no unbounded queue.
- In-flight frames: old swapchain retirement, no use-after-free, no validation errors.
- Style/layout: viewport/media/container re-evaluation, text reflow, percentages/flex/grid.
- Render scale: the 96/120/144/192 dpi table plus in-between and tie scales, per-window state
  transitions (repeat snapshots, same-step DPI changes, invalid metrics, monitor changes), and
  per-scale atlas keying, invalidation and re-rasterization (`tests/unit/scale/`, ctest names
  `scale_quantize`, `scale_state`, `scale_atlas`).
- Animation: retarget (not restart), reduced motion, no layout feedback loop.
- Platform integration: Windows modal resize, X11 sync counter, Wayland configure/ack.
- Determinism: same synthetic sequence -> same layout/style dump and pixels.

## 6. Open questions

- Per-platform overrides of `resizeMode` (the default is `adaptive` on all platforms per [`runtime-config-v1.md`](../specs/runtime-config-v1.md); no per-platform override is decided).
- Exact swapchain-recreate delay heuristic for slow GPUs.
- Safe-area APIs for Android/macOS (both backends are unscheduled; macOS per ADR-010, Android in Big Step F).
- Whether to expose a declarative `onResize`/`ResizeObserver` API or keep it internal. Register
  answer: keep it internal until container queries ship. Superseded by proposal:
  [ADR-041](../../decisions/ADR-041-layout-observation.md) (Accepted) adds a post-layout `onSizeChange`
  observer and a layout read model first ([`layout-observation.md`](layout-observation.md)).
- ~~Fractional scaling + text rasterization on mixed-DPI multi-monitor setups.~~ Decided: the
  device scale quantizes onto the 1.0/1.25/1.5/2.0 ladder (nearest, ties up, clamped) and each
  ladder step owns one glyph atlas that is released when the window leaves that step (§2.3). One
  render scale per window; per-monitor scale stays out of V1
  ([`performance.md`](performance.md) §10).

## 7. Device loss (D7)

Backend detection, the recovery API, and the generation-guard rules are specified in
[`gpu/backends.md`](gpu/backends.md#81-device-loss-d7); this section is the frame/lifecycle side.

Observed state (the only values UI/runtime code sees):

- `Device::status()` becomes `Lost`; swapchain acquire/present return
  `SwapchainError::DeviceLost`; command-list waits report incomplete. No exception crosses the
  backend seam on the loss path and no wait becomes unbounded (ADR-021).
- A lost frame is dropped, not retried inside the OS callback or the present call.

Runtime orchestration (implemented): `runtime::DeviceRecoveryController`
([`device_recovery.hpp`](../../include/tessera/runtime/device_recovery.hpp)) is polled once per
`RuntimeLoop::tick()` when attached via `RuntimeLoopConfig::device_recovery` /
`RuntimeLoop::set_device_recovery`. States are explicit and drive the loop:

| State | Meaning | Loop behaviour |
|---|---|---|
| `Ready` | device healthy | normal render/pacing |
| `WaitingForCaller` | lost, and `recover()` refused with `ResourcesOutstanding` | frames stopped; one cheap probe per tick; caller destroys lost-generation RHI objects |
| `Recovering` | the tick's single bounded `recover()` call is in flight | never observed across ticks |
| `Failed` | the one-shot recovery failed (or `NotSupported`) | frames stopped; no further attempts until the device is observed `Ready` and then `Lost` again |

Loop contract:

- While the controller requires recovery, `RuntimeTick::device_blocked` is set,
  `should_render` is false (no present), and `RuntimeTick::idle` is true so the caller idles on the
  existing bounded pacer/platform wait (`runtime::wait_for_next_frame`) instead of spinning ticks.
  The loop never blocks inside `tick()`; the controller makes at most one recovery attempt per
  tick and reports outcomes as values.
- On the tick a recovery succeeds, `RuntimeTick::device_recovered` is set, `device_generation`
  bumps, and `full_frame` is true. The caller recreates the swapchain and every RHI object before
  the next render; the next tick is the one-shot full-frame repaint (`full_frame` true once more,
  damage/redraw state is invalid), then normal on-demand pacing resumes.
- A failed recovery sets `device_failed` and the controller stays `Failed`; another attempt only
  happens after the device is observed `Ready` again and lost anew (the backend's one-shot policy).
- Callers that do not attach a controller keep the previous behaviour; device loss is then their
  responsibility.

Frame-boundary steps for the caller:

1. Stop submitting and presenting; never call a GPU wait from a message callback. Every existing
   wait stays bounded.
2. Destroy the swapchain and every RHI object from the lost generation. Destructors are
   generation-guarded, but `recover()` refuses (`ResourcesOutstanding`) until they are gone.
3. Let the controller's tick drive `Device::recover()` (one bounded call per tick); do not call it
   in a loop or from the OS callback.
4. On `device_recovered`, recreate resources -> swapchain -> pipelines/targets from
   `device_generation`, then render the flagged full frame and resume the loop.

Per-platform UX policy (error surface while `Lost`, retry cadence, telemetry, whether to block the
shell) stays an application/runtime policy decision. Out of scope for D7: multi-device and
multi-swapchain orchestration, surface recreation policy, allocation-failure handling, and real
GPU-reset coverage across drivers/vendors (the fault-injection tests cover the state machine).

### 7.1 Present-path hardening demo and smoke (task-50)

`batched2d_poc --windowed` has two opt-in hardening modes:

- `--msaa 4 --font <fixture>`: swapchain images are always 1-sample, so the POC renders the scene
  (rounded clip + rounded rect + wrapped paragraph + rotated quad) into an **offscreen 4x target**
  whose pass resolves to its 1-sample image, then bridges that resolved image into the present image
  through a CPU readback -> 1x texture -> fullscreen textured quad. The bridge exists because the RHI
  has no image-to-image copy yet (`CommandList` only copies a target to a buffer); it is a demo
  workaround, not the intended production path. Readback bytes are in the target's encoded space, so
  on an sRGB swapchain the extra sample->encode->decode step is a documented visual approximation;
  the path is validation-clean. A future RHI `blit`/resolve-to-image would remove the CPU bridge.
- `--simulate-loss --font <fixture>` (requires a `TESSERA_VULKAN_TEST_HOOKS` build): after three
  rendered frames the POC injects a simulated device loss, destroys every RHI object (swapchain,
  pipelines, targets, textures, text atlas textures), lets `runtime::DeviceRecoveryController` drive
  recovery through the normal `RuntimeLoop` tick, recreates resources on `device_recovered`, and
  renders three more frames. It prints `frames_before/after`, `blocked_ticks`, the recovery tick and
  generation, and fails if no frame renders after recovery.

Measured locally (AMD RX 5300M, validation on): `--msaa 4` = 3/3 frames presented; combined
`--msaa 4 --simulate-loss` = 3 frames before + loss injected + recovered at tick 4 (generation 1) +
3 frames after, 0 blocked ticks, no validation messages.

`tests/gpu/vulkan/present_hardening_tests.cpp` is the CI smoke: 3 non-MSAA frames of the same scene
must be byte-identical (and a repeat run matches), a 4x frame renders through its resolved image when
supported, and a simulated loss recovered by the controller must render the pre-loss pixels again.
It skips cleanly without Vulkan, test hooks, or the fixture font, and fails on any `[vulkan]` line.

### 7.2 App window recovery (`WindowHost`)

The production window driver ([`include/tessera/app/window.hpp`](../../include/tessera/app/window.hpp))
attaches a `runtime::DeviceRecoveryController` to its `RuntimeLoop` and recovers instead of ending
the run. The caller side of one loss:

1. **Blocked** (`RuntimeTick::device_blocked`, or a frame whose acquire/present reported
   `SwapchainError::DeviceLost`): no frame is begun or presented. The first blocked tick of a loss
   releases the lost generation - the in-flight command lists first (closed out without polling: a
   lost device never signals its fences, so polling would spin the UI thread) and then the sink,
   renderer, text stack (glyph-atlas textures included), white texture, pipeline and swapchain. The
   driver then parks on the bounded platform wait, so recovery never spins.
2. **Recovered** (`RuntimeTick::device_recovered`): the device is healthy again. The driver recreates
   the whole presentation stack for the window's *current* metrics: swapchain (image count and
   `WindowOptions::vsync`), the 2D pipeline for the swapchain's format, the white texture, the
   `Batched2DRenderer` (two frames in flight), the `TextStack` (face, rasterizer, shaper, atlas) and
   the `Batched2DSink`, then calls `RuntimeLoop::mark_render_dirty()` so the next frame is a full
   repaint - the damage plan and scissor state described the lost swapchain generation.
3. **Failed** (`RuntimeTick::device_failed`): the backend's one-shot in-place `recover()` is spent,
   so the driver replaces the RHI device object itself (`detail::create_device()`), reattaches the
   controller to the new device and recreates the stack. The platform window outlives every device
   generation, so its native surface handle, the input queue (`Window::set_input_queue`) and the
   accessibility root seam stay valid; the queue is re-asserted after a recovery.

Attempt budget (explicit, [`include/tessera/app/recovery_policy.hpp`](../../include/tessera/app/recovery_policy.hpp)):
`kMaxDeviceRecoveryAttempts = 3` per `run()`. Each release/recovery cycle and each device rebuild
charges one attempt; when the budget is spent - or a rebuild cannot create a device or a swapchain -
the run stops through `FrameFailureGuard` (`stats.stalled == true`,
`stats.device_state == WindowDeviceState::Lost`). The budget is per `run()` invocation, not per loss,
so a caller that retries `run()` starts a fresh bounded sequence.

Nothing device-wide is waited on from the UI thread: no `Device::wait_idle()`. The only device-wide
wait is the backend's own teardown inside `recover()` (its simulated-loss path), and the driver's
in-flight drain is a bounded per-list poll that is skipped entirely once the device is lost.

What the caller sees (`WindowFrameStats`):

| Field | Meaning |
|---|---|
| `recovered` | successful recoveries in this run (each recreated the presentation stack) |
| `recovery_attempts` | attempts charged against the budget |
| `device_state` | `Ready`, `Lost` or `Unavailable` when `run()` returned |
| `stalled` | the run gave up; `device_state` is `Lost` |

Tests: `tests/unit/app_recovery_policy_tests.cpp` (ctest `app_recovery_policy`) drives the policy
deterministically - a transient loss releases once and recovers, a persistent loss lands exactly on
the budget and gives up, a zero budget gives up immediately. `tests/gpu/app_window_recovery_tests.cpp`
(ctest `app_window_recovery`) injects one `DeviceLostOnAcquire` fault through the Vulkan backend's
fault seam on a real hidden window, runs two frames and asserts the run recovered, presented,
reported `Ready`, stayed inside the budget and kept presenting afterwards. It skips without a window
backend, the Vulkan backend or the fault hooks. Real driver resets (a dead `VkDevice`, not a
simulated one) remain unproven without hardware fault injection.
