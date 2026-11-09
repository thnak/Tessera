# HAL and platform layer

Status: the B2 timer and clipboard seams are implemented (`tessera_hal_timer`,
`tessera_hal_clipboard`); the rest of
`src/hal/` and `src/platform/` is design for Big Step B3 (window/swapchain path; see
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision record:
[ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md) (window resize and lifecycle; OS
compliance lives in the HAL) and
[ADR-023](../../decisions/ADR-023-os-event-normalization-and-foldables.md) (OS event normalization
and foldables).
Mobile-specific surface/input/IME/accessibility/declarations are in
[`mobile-integration.md`](mobile-integration.md).

The HAL (hardware/OS abstraction layer) is the only place that knows about operating-system APIs. It normalizes window,
display, input, lifecycle, power, timer, filesystem, graphics-surface, and audio signals into
Tessera types. The UI layer never receives a `HWND`, `xcb_window_t`, `wl_surface*`, `ANativeWindow*`,
or `NSWindow*`.

## 1. Responsibilities

| Area | HAL interface | Platform sources |
|---|---|---|
| Window | create/show/hide/close, title, size constraints, fullscreen, focus | Win32, X11, Wayland, Cocoa, Android |
| Display | monitors, modes, refresh/VRR, scale/DPI, HDR/color, safe/work area | DXGI/DisplayConfig, RandR, wl_output, Android DisplayManager, NSScreen |
| Input | pointer, keyboard, touch, pen, wheel, IME, focus/capture | WM_POINTER/WM_KEY, XInput2/XKB, wl_seat, Android MotionEvent, NSEvent |
| Lifecycle | activate/deactivate, show/hide, minimize/restore, suspend/resume, lock/unlock, shutdown | WM_*, X11/Wayland, Android Activity/scene, Cocoa |
| Power | AC/battery, battery saver, thermal, screen on/off | power GUIDs, logind/UPower, Android PowerManager, NSProcessInfo |
| Timer | monotonic clock, bounded idle wait, frame deadlines | `src/hal/timer/` (portable), window event wait in §1.2 |
| Filesystem | paths, watch, atomic write, asset roots | Win32, inotify/FSEvents, Android assets |
| Graphics | surface creation/destruction, resize handshake | Vulkan surface extensions per platform |
| Audio | optional notification/UI sounds | miniaudio (optional; see [`dependencies.md`](../specs/dependencies.md)) |

The detailed OS event mapping is normative in [`os-events-v1.md`](../specs/os-events-v1.md).
Resize/DPI/lifecycle rules are in [`window-resize.md`](window-resize.md).

### 1.1 Timer seam (implemented)

`src/hal/timer/steady_time.*` is the first implemented HAL module (`tessera_hal_timer`, B2):

- `steady_now_seconds()` returns monotonic seconds from a fixed epoch whose origin is arbitrary
  (typically boot), backed by `std::chrono::steady_clock`; values may be compared and subtracted
  but are not wall-clock time.
- `seconds_until(now, deadline)` is the pure boundary helper: `max(0, deadline - now)`; a NaN
  comparison yields `0`.
- `wait_until_seconds(deadline)` performs one bounded sleep and never spins. It returns immediately
  when the deadline is already past or the remaining duration is non-finite.

The UI thread may idle in `wait_until_seconds` between ticks because the wait is bounded by the
next deadline and holds no locks; [ADR-021](../../decisions/ADR-021-async-runtime.md) treats this
as idling rather than blocking. Early wakeups from input, async completions, and
`request_frame()` are provided by the B3 window event wait (§1.2), not by this portable primitive.
`runtime::SystemClock` (`src/runtime/system_clock.*`) is the only runtime adapter and delegates
here; `src/hal/` never depends on `src/runtime/`.

### 1.2 Window event wait (implemented, Windows and Linux/X11)

`hal::Window::wait_for_events(timeout_seconds)` idles on the owning thread while pumping OS
messages and returns `true` when at least one normalized `WindowEvent` is queued (or a close or
coalesced redraw wake is pending), `false` on timeout. Negative or non-finite timeouts are clamped
to zero and every wait is bounded. On Windows the wait also returns `true` while the application's
input queue (`set_input_queue`, see [`input.md`](input.md) section 2.1) holds unconsumed input.
The Win32 implementation (`src/platform/windows/window.cpp`)
waits with `MsgWaitForMultipleObjectsEx` (`QS_ALLINPUT`, bounded millisecond timeout) and then
drains with `PeekMessage`/`DispatchMessage`, preserving queue order and resize coalescing. The
Linux implementation (`src/platform/linux/window.cpp`) `poll()`s the Xlib connection fd with a
millisecond timeout rounded up from the remaining budget, then drains `XPending`/`XNextEvent`; a
foreign-thread call returns `false` and a dead connection idles out the remaining budget once
instead of spinning. Both platform paths only record events, so no user code runs from inside the
wait.
Platforms without an event source (macOS until its backend lands, and Linux configured without
X11) keep the bounded `wait_until_seconds` fallback that returns `false`.

[`include/tessera/hal/window_wait.hpp`](../../include/tessera/hal/window_wait.hpp) adds
`wait_for_events_until(window, deadline_seconds)`: it slices the platform wait so a wakeup cannot
overshoot `deadline_seconds` and returns `WaitOutcome::Event` as soon as the window reports an
event, or `WaitOutcome::Deadline` otherwise (a past deadline returns immediately). Runtime
integration should idle between ticks by computing the next `FramePacer` deadline and calling
`wait_for_events_until(window, deadline)`; `RuntimeLoop::tick()` behavior is unchanged until that
runtime slice lands.

### 1.3 Clipboard seam (implemented)

`include/tessera/hal/clipboard.hpp` (`tessera_hal_clipboard`) is the only clipboard surface UI code
sees; editable text never calls an OS API directly:

- `Clipboard::read_text()` / `write_text(std::string_view)` are UTF-8-only and return
  `std::expected` values. Both directions are bounded by `kMaxClipboardBytes` (64 KiB): a larger
  write fails with `TooLarge`, a larger native read fails with `TooLarge` rather than truncating.
  An empty clipboard reads as an empty string.
- Failure values: `Unsupported` (no backend for this platform/session), `Busy` (contention beyond
  the retry budget), `TooLarge`, `Failed`. Failures never throw and never block indefinitely.
- Backends: `MemoryClipboard` (deterministic tests / headless), `UnsupportedClipboard` (reports
  `Unsupported` for both operations), and `create_native_clipboard()`. Win32 implements
  `CF_UNICODETEXT` with bounded UTF-16 <-> UTF-8 conversion and a bounded `OpenClipboard` retry:
  at most 4 attempts with 1 ms between them (<= 3 ms), then `Busy` - the UI thread never spins.
  Linux/X11 (selection protocol), macOS (`NSPasteboard`) and Android (ClipboardManager bridge)
  currently report `Unsupported`; their slices are deferred.
- The edit controller consumes only this seam (`EditController::set_clipboard` wires the Ctrl+C/X/V
  chords; the copy/cut/paste commands take a `Clipboard&`), so tests and headless hosts inject the
  in-memory backend.

## 2. Core types

```cpp
struct WindowMetrics {
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t physical_width = 0;
    uint32_t physical_height = 0;
    float dpi_scale = 1.0f;
    float refresh_hz = 60.0f;
    Rect safe_area;
    Rect work_area;
    Orientation orientation = Orientation::Landscape;
    bool focused = true;
    bool occluded = false;
    bool minimized = false;
    bool fullscreen = false;
    bool multi_window = false;
    bool picture_in_picture = false;
    DisplayId display = 0;
    FoldInfo fold;
};

struct DisplayInfo {
    DisplayId id;
    std::string name;
    Rect bounds;                 // compositor/logical coordinates
    Extent2D physical;
    float scale = 1.0f;
    float refresh_hz = 60.0f;
    bool hdr = false;
    ColorGamut gamut = ColorGamut::Srgb;
    bool primary = false;
    bool vrr = false;
};

struct PowerState {
    PowerSource source = PowerSource::Unknown;
    float battery_percent = -1.0f;
    bool battery_saver = false;
    ThermalState thermal = ThermalState::Unknown;
    bool screen_on = true;
    bool session_locked = false;
};
```

`WindowMetrics` is the only geometry/display contract the UI sees (the same struct is also shown in
[`window-resize.md`](window-resize.md) section 1 with different field types for logical size). `FoldInfo` is defined in
[`os-events-v1.md`](../specs/os-events-v1.md).

## 3. Interfaces (target shape)

```cpp
class WindowSystem {
public:
    virtual ~WindowSystem() = default;
    virtual std::unique_ptr<Window> create_window(const WindowDesc&) = 0;
    virtual std::span<const DisplayInfo> displays() const = 0;
    virtual void poll_events(EventQueue&) = 0;   // never blocks
};

class Window {
public:
    virtual ~Window() = default;
    virtual WindowMetrics metrics() const = 0;
    virtual void set_title(std::string_view) = 0;
    virtual void set_size(Extent2D logical) = 0;
    virtual void request_close() = 0;
    virtual GraphicsSurface& surface() = 0;
};

class GraphicsSurface {
public:
    virtual ~GraphicsSurface() = default;
    virtual SurfaceDesc desc() const = 0;
    virtual void resize(Extent2D physical, float scale) = 0;
    virtual void retire_old() = 0;              // after frames in flight
};
```

These are target interfaces, not implemented headers. Keep them small; add a method only when
a real feature needs it ([ADR-003](../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md), thin RHI and batched 2D
renderer, whose "thin" rule applies to the HAL too; RHI = render hardware interface).

## 4. Event delivery

```text
platform callback -> record raw event (no user code)
  -> normalize -> coalesce -> queue
  -> UI thread drains at tick/frame boundary (tick = one fixed-step update, [`game-loop.md`](game-loop.md))
  -> update metrics/state -> mark dirty -> wake loop if visible
```

- No layout, render, file I/O, or blocking wait inside a platform callback.
- Coalesce resize/move/DPI/refresh/pointer-move; never drop lifecycle/fold/power transitions.
- Delivery order is stable: lifecycle -> display/DPI -> fold -> input -> style/layout/paint.
- Unknown events are ignored with a diagnostic, never a crash.
- Deterministic mode injects synthetic events and does not call platform APIs.

## 5. Platform matrix

| Capability | Windows | X11 | Wayland | Android | macOS |
|---|---|---|---|---|---|
| Window create/show | Win32 | Xlib/xcb | xdg-shell | Activity/native window | Cocoa |
| Resize/DPI | `WM_SIZE`, `WM_DPICHANGED` | `ConfigureNotify`, RandR | `xdg_toplevel.configure` + `ack` | `onConfigurationChanged`, `WindowMetrics` | `NSWindowDelegate` |
| Display topology | DisplayConfig, `WM_DISPLAYCHANGE` | RandR | `wl_output`/`zxdg_output_v1` | `DisplayManager` | `NSScreen` |
| Refresh/VRR | DXGI/DisplayConfig | RandR/Present | `wl_output`, `wp_presentation` | `Display.Mode`, `Surface.setFrameRate` | `NSScreen.maximumFramesPerSecond`, CVDisplayLink |
| Power/thermal | power GUIDs, `WM_POWERBROADCAST` | logind/UPower | logind | `PowerManager` | `NSProcessInfo` |
| Fold/posture | no standard hinge API | no standard | compositor-specific | Jetpack WindowManager | no mainstream foldable |
| IME | `WM_IME_*` | XIM/IC | `zwp_text_input_v3` | `InputConnection` | `NSTextInputClient` |
| Clipboard (UTF-8 text) | Win32 `CF_UNICODETEXT` | unsupported stub (selection deferred) | unsupported stub | deferred | deferred |

No capability may be assumed. Missing data uses a documented fallback (60 Hz, scale 1.0, no
VRR, no HDR) and records the assumption in metrics.

## 6. Surface and swapchain handshake

```text
OS resize/DPI -> WindowMetrics
  -> renderer decides resize mode
  -> create new surface/swapchain (or offscreen target)
  -> keep old resources until in-flight frames retire
  -> swap render target
  -> request one frame
```

- Zero-size/minimized: do not create a target; mark pending.
- Live resize: coalesce; follow `resizeMode` (`live`, `deferred`, `adaptive`).
- Wayland: never draw before `ack_configure`; the compositor owns the size.
- Windows: use the `WM_DPICHANGED` suggested rect; do not fight the modal resize loop.
- X11: `_NET_WM_SYNC_REQUEST` is a hint; never block waiting for it.
- Old swapchains/targets retire only after their frames-in-flight complete.

See [`window-resize.md`](window-resize.md) for the state machine and
[`async.md`](async.md) for the no-blocking rules.

## 7. Threading

- HAL event polling and UI mutation happen on the UI thread.
- Platform callbacks may run on OS threads; they only enqueue normalized events.
- Surface creation/destruction may block in the OS/GPU driver; do it off the hot path and
  outside layout/paint.
- Never call `vkDeviceWaitIdle`/`vkWaitForFences` indefinitely on the UI thread.
- Worker tasks never touch the scene graph or window handles.

## 8. Errors and diagnostics

- A missing platform feature degrades; it does not crash the app.
- Surface/device loss is reported as a recoverable event; the runtime recreates resources.
- Every fallback (60 Hz/1.0 scale/no VRR) is recorded in metrics.
- Validation-layer errors are diagnostics in debug builds.
- Platform-specific errors are translated to Tessera error codes; raw OS codes are attached
  for debugging and redacted from user-facing messages.

## 9. Tests

- Synthetic event scripts per platform: resize, DPI, display hotplug, refresh/VRR, fold,
  suspend/resume, lock/unlock, IME, power/thermal.
- A fake HAL for unit tests: deterministic `WindowSystem`/`Window`/`GraphicsSurface`.
- Timer seam: pure boundary math, prompt return for past deadlines, and a non-decreasing clock,
  without real sleeps for scheduling assertions.
- Golden mode (deterministic reference-image tests, [`golden-tests-v1.md`](../specs/golden-tests-v1.md)) never calls platform APIs.
- Platform smoke tests run in CI on Windows and Linux (the only platforms with a build in AGENTS.md Definition of done).
- Multi-monitor/DPI and fold sequences are explicit test fixtures.

## 10. Current status

- `src/hal/timer/` is implemented (`tessera_hal_timer`): monotonic clock + bounded idle wait.
- `src/hal/clipboard/` is implemented (`tessera_hal_clipboard`): UTF-8 text seam bounded at 64 KiB
  with `MemoryClipboard`/`UnsupportedClipboard` backends and the Win32 `CF_UNICODETEXT` backend
  (bounded retry; Linux/X11, macOS and Android report `Unsupported` this slice). It is registered
  from `src/hal/window/CMakeLists.txt` until the root CMakeLists lists the directory directly.
- `src/hal/window/` provides `tessera_hal_window` and `hal::create_window`. The Win32
  implementation lives in `src/platform/windows/window.cpp`; the X11 implementation lives in
  `src/platform/linux/window.cpp` and is selected when CMake finds Xlib. Other platforms, and
  Linux configurations without X11, build a stub that returns `nullptr`. Window creation,
  normalized resize/DPI/focus/close events, owner-thread destruction, and the event-driven
  `wait_for_events` idle wait are covered by `window_win32`/`window_wait` on Windows and
  `window_x11` on Linux (skipped when `DISPLAY` is unset). `include/tessera/hal/window_wait.hpp`
  supplies the deadline-bounded `wait_for_events_until` helper for the runtime/frame pacer.
  Wiring that helper into `RuntimeLoop` is still pending; the loop's tick behavior is unchanged.
- Platform `VkSurfaceKHR`/swapchain paths live in `src/rendering/backend/vulkan/src/` and are
  created through `Device::create_swapchain` with the opaque `native_window` (HWND) or
  `native_display` + `native_window` (`Display*` + `Window`) handles supplied by the HAL. Win32
  and Xlib are implemented; other configurations report `NotSupported`. The Xlib smoke test runs
  under Xvfb with lavapipe.
- B1 is offscreen; the B3 window/swapchain path is Windows + Linux/X11. Remaining B3 slices are
  Wayland, swapchain-image render targets, per-image frames-in-flight synchronization, live
  resize/device-loss policy, and runtime/pacer integration (macOS via
  [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md); Android per
  [`mobile-integration.md`](mobile-integration.md)).
- X11 limitations in this slice: no Wayland, no IME/XIM, no occlusion/minimize signal, no RandR
  or display-hotplug handling, and DPI is a global `Xft.dpi` heuristic (96 fallback). See
  [`window-resize.md`](window-resize.md) §0.
- Mobile/foldable support follows [`os-events-v1.md`](../specs/os-events-v1.md).
