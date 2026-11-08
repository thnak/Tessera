# Window resize, DPI, and lifecycle

Status: design for Big Step B3 (window/swapchain path; B3 and the other Big Steps are defined in
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision record:
[ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md) (window resize and lifecycle).
This document draws the line between
**declarative style/animation** and **window/renderer infrastructure**. HAL = hardware/OS abstraction
layer ([`hal.md`](hal.md)); tick, golden, VRR and `WindowMetrics` are in
[`glossary.md`](../reference/glossary.md).

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
    float logical_width = 0;
    float logical_height = 0;
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
  with a generation counter; it is not the raw OS resize event.
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
- Live-resize storms: coalescing, one resize frame per tick, no unbounded queue.
- In-flight frames: old swapchain retirement, no use-after-free, no validation errors.
- Style/layout: viewport/media/container re-evaluation, text reflow, percentages/flex/grid.
- Animation: retarget (not restart), reduced motion, no layout feedback loop.
- Platform integration: Windows modal resize, X11 sync counter, Wayland configure/ack.
- Determinism: same synthetic sequence -> same layout/style dump and pixels.

## 6. Open questions

- Default `resizeMode` per platform (Windows modal loop vs Wayland compositor-driven).
- Exact swapchain-recreate delay heuristic for slow GPUs.
- Safe-area APIs for Android/macOS (both backends are unscheduled; macOS per ADR-010, Android in Big Step F).
- Whether to expose a declarative `onResize`/`ResizeObserver` API or keep it internal.
- Fractional scaling + text rasterization on mixed-DPI multi-monitor setups.
