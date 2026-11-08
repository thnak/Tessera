# HAL and platform layer

Status: design for Big Step B3. `src/hal/` and `src/platform/` are placeholders today.
Mobile-specific surface/input/IME/accessibility/declarations are in
[`mobile-integration.md`](mobile-integration.md).

The HAL is the only place that knows about operating-system APIs. It normalizes window,
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
| Timer | monotonic clock, sleep/wakeup, frame deadlines | OS timers + runtime loop |
| Filesystem | paths, watch, atomic write, asset roots | Win32, inotify/FSEvents, Android assets |
| Graphics | surface creation/destruction, resize handshake | Vulkan surface extensions per platform |
| Audio | optional notification/UI sounds | miniaudio (optional) |

The detailed OS event mapping is normative in [`os-events-v1.md`](../specs/os-events-v1.md).
Resize/DPI/lifecycle rules are in [`window-resize.md`](window-resize.md).

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

`WindowMetrics` is the only geometry/display contract the UI sees. `FoldInfo` is defined in
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
a real feature needs it (ADR-003 "thin RHI" applies to the HAL too).

## 4. Event delivery

```text
platform callback -> record raw event (no user code)
  -> normalize -> coalesce -> queue
  -> UI thread drains at tick/frame boundary
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
- Golden mode never calls platform APIs.
- Platform smoke tests run in CI where possible (Windows/Linux first).
- Multi-monitor/DPI and fold sequences are explicit test fixtures.

## 10. Current status

- `src/hal/*` and `src/platform/*` contain placeholders only.
- B1 is offscreen; no window or swapchain.
- B3 implements the window/swapchain path and native platform backends.
- Mobile/foldable support follows [`os-events-v1.md`](../specs/os-events-v1.md).
