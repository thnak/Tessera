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
| Window | create/show/hide/close, title, size constraints, full-screen (Win32/X11 implemented; Wayland window + surface #155), custom-caption regions, focus | Win32, X11, Wayland, Cocoa, Android |
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
as idling rather than blocking. Early wakeups from input and `request_frame()` are provided by the
B3 window event wait (§1.2), not by this portable primitive; async completions joined it with
issue #842 - see the wake paragraph in §1.2.
`runtime::SystemClock` (`src/runtime/system_clock.*`) is the only runtime adapter and delegates
here; `src/hal/` never depends on `src/runtime/`.

### 1.2 Window event wait (implemented, Windows and Linux/X11)

**Completion wake (issue #842).** A worker that posts a completion has to end a parked UI thread
early, or every async result in a settled app arrives at the idle clamp (`kMaxIdleWaitSeconds`,
up to 250 ms) rather than when it is ready. `hal::WakeSource` (`include/tessera/hal/wake_source.hpp`)
is the primitive the host installs: Linux joins an `eventfd` beside the display fd in `poll()`,
Windows adds a manual-reset event (`wake_source_win32.cpp`; `drain()` clears it, so a wait observes the signal without consuming it) to the `MsgWaitForMultipleObjectsEx` handle list, and a window-less
group waits on it directly. `CompletionQueue::set_wake` fires it once per empty -> non-empty edge,
so a burst is one wake. The wake only ends the park; the next tick still drains the queue on the UI
thread, and the clamp stays as the safety net rather than the latency path. A platform with no
primitive returns null from `create_wake_source`, which is the documented bounded-sleep fallback.
Measured wake-to-return latency of a parked window wait under Xvfb (`window_x11`, 20 trials, a burst of three wakes each): X11 median 0.05 ms, max 0.07 ms. On Windows (`window_wait`, the same 20 trials and burst, Release, local desktop) the median is 0.02 ms and the worst trial 0.04 to 17 ms across runs, so a wake ends the park in well under one pacer interval rather than at the 250 ms clamp. The X11 window did not override `set_wake_source` until this was measured, so the eventfd never joined its poll; the `window_x11` test now guards it. A window-less group (the tray app) is covered by `app_window_tray`: it parks on the wake and a worker's post ends the park in milliseconds. That test also found two defects in `WindowDevice::run_windows`: with no window the pacer never presents, so `next_wait_seconds()` answered "render now" forever and the group spun at full CPU instead of parking, and a stop requested by a completion still waited out one idle park. Both are fixed.

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

### 1.4 Display refresh query (implemented, Windows and Linux/X11)

`hal::Window::display_refresh_hz()` is the window's only display-refresh source (runtime-config-v1.md
section 6.2). It returns Hz, or the documented "unknown" value `kUnknownRefreshHz` (0): a platform
without a query, a failed OS call, or a value outside the sane bounds of
[`refresh_rate.hpp`](../../include/tessera/hal/refresh_rate.hpp) is unknown, never a guess, and the
call never throws. The value handling, the sane bounds and the explicit-option precedence are pure
functions in that header, so tests cover them without a display; a window's implementation is only
the OS read. Windows reads the current mode of the monitor the window is on
(`MonitorFromWindow`/`GetMonitorInfoW`/`EnumDisplaySettingsExW`), Linux/X11 reads the active RandR
CRTC mode when the extension is present (`TESSERA_X11_RANDR`; a build without libXrandr compiles the
unknown path), and every other platform reports unknown. It is not a per-frame query: the app window
host reads it once at creation, and an explicit `WindowOptions::refresh_hz` wins over it. Monitor
changes are not tracked in this slice; the OS-event display-capabilities item in
[`os-events-v1.md`](../specs/os-events-v1.md) owns that.

### 1.5 Cursor shape (implemented, Windows and Linux/X11)
`hal::Window::set_cursor(hal::CursorShape)` is the pointer-shape seam (window-chrome-v1.md section
8.5, issue #191); `supports_cursor()` says whether the platform can do it at all, and
`WindowMetrics::cursor` reports the shape the platform resolved, not the last request. `CursorShape`
is the app-visible intent (`Default` is the platform arrow, `Hidden` is CSS `cursor: none`) and the
platform decides its OS call: X11 creates a `cursorfont` glyph with `XCreateFontCursor`, caches it per
window and installs it with `XDefineCursor`; Win32 loads the stock `IDC_*` cursors and installs the
current shape from `WM_SETCURSOR` (the OS re-asserts the cursor after every non-client transition). The
call never blocks, throws or fails: an unsupported shape or platform leaves the pointer as it was, and
the app reports one `TESSERA_WINDOW_CURSOR_UNSUPPORTED`. The value names are pure
([`cursor.hpp`](../../include/tessera/hal/cursor.hpp) plus `src/hal/window/cursor.cpp`), so the
metrics dump and the app's mapping are testable without a display; the platform halves are
`src/platform/windows/window_cursor_win.*` and `src/platform/linux/window_cursor_x11.*`. Tests:
`window_cursor`, `window_cursor_fake`, `window_win32`, `window_x11`.

### 1.6 Display capabilities and normalized events (implemented, Windows and Linux/X11)

The first ADR-023 slice is `include/tessera/hal/os_event.hpp` (`OsEventType`, `OsEvent`,
`OsEventSequence`), `include/tessera/hal/display.hpp` (`WindowId`/`DisplayId`, `DisplayInfo`,
`Orientation`, `ColorGamut`, and the documented fallback helpers) and
`include/tessera/hal/event_reason.hpp`. The normative rules are in
[`os-events-v1.md`](../specs/os-events-v1.md) sections 2-3; this section records the shape.

`WindowMetrics` carries the capability half as plain values, with an "assumed" bit next to every one
the platform may not be able to measure: `display`, `refresh_hz`/`assumed.refresh_hz`,
`vrr`/`assumed.vrr`, `work_area`/`assumed.work_area`, `orientation`/`assumed.orientation`, plus
`multi_window` and `picture_in_picture`. The four bits share one byte (`DisplayAssumptions`) instead
of four separate `bool`s, because the metrics are copied into every event: the padding would
otherwise cost three bytes per copy. `kAssumedDisplayRefreshHz` (60) and `kAssumedDisplayScale` (1.0)
are the documented fallbacks (ADR-023 rule 7) and the flags are what stop a fallback being read as a
measurement. There is **no** separate `DisplayCapabilities` type: the metrics are the per-event
snapshot, copied with every event, so they stay cheap and allocation-free. The per-monitor topology
is the separate `DisplayInfo` list `hal::Window::displays()` fills (at most `kMaxDisplays`), re-read
once per tick after a `DisplayAdded`/`DisplayRemoved`/`DisplayChanged` event - never polled per frame.

`kWindowMetricsSizeBudget` (384 bytes) is a `static_assert` on `sizeof(WindowMetrics)`: the struct is
copied by the platform event ring, the pump path and `OsEvent`, so it is a hot-path cost in the same
way `kNodeSizeBudget` guards `Node`. It has headroom for a few more capability fields; growing past it
needs a deliberate decision rather than an accidental struct member.

`WindowEvent` gained `EventReason` and a monotonic `sequence`; `WindowEventType` gained
`RefreshRateChanged`, `WorkAreaChanged`, `DisplayAdded`, `DisplayRemoved`, `DisplayChanged` and
`ScaleChanged`. The window path is a projection of the `OsEvent` taxonomy: `os_event_type_for` maps
every enumerator (table-driven, so an unmapped one is a compile error), and the delivery order of
section 3 is derived from the mapped `OsEventType` rather than duplicated.

Platform halves, all of which only record:

- Windows (`src/platform/windows/window_display_win.*`, `window_display_messages_win.*`,
  `window_dpi_win.*`): `GetMonitorInfoW` + `EnumDisplaySettingsExW` + `GetDpiForMonitor` fill the
  metrics (work area, refresh, per-monitor scale, orientation) and `displays()`; the monitor device
  interface (`RegisterDeviceNotificationW`) plus `WM_DISPLAYCHANGE`/`WM_DPICHANGED`/
  `WM_SETTINGCHANGE` become the normalized events.
- Linux/X11 (`src/platform/linux/x11_display.*`, `x11_randr_monitors.*`): `_NET_WORKAREA` for the
  work area, RandR for the monitor list and the active CRTC's refresh, with
  `RRScreenChangeNotify`/`RRNotify` selected at creation (and `XRRUpdateConfiguration` before any
  RandR read).

The app driver (`src/app/window_lifecycle.cpp`) takes one metrics snapshot at the tick boundary,
follows a refresh change through the pacer's existing rate-change path (no stall, no swapchain
recreation), and re-reads the topology once per tick. The synthetic injector
(`include/tessera/hal/synthetic_events.hpp`) provides the deterministic scripts: resize/DPI storm,
display hotplug, scale change and refresh change.

Tests: `os_event` (the pure rules and the mapping) and `os_event_display` (the scripts through the
fake window). Both are OS-free and run on the Windows and Linux gating lanes; the real platform reads
are covered by `window_win32`/`window_x11`.

### 1.7 OS appearance: color scheme and reduced motion (implemented, Windows and Linux/X11)

Issue #187 completes the settings half of ADR-023 on Linux and adds the color-scheme half on both
platforms. The surface is `include/tessera/hal/appearance.hpp`:

- `ColorSchemePreference` (`Unspecified`, `Light`, `Dark`, `NoPreference`) and
  `AppearancePreference` (one color-scheme value plus a `reduced_motion` flag) are the normalized
  snapshot. `Unspecified` is the "the platform did not answer" value, distinct from the portal's
  `NoPreference` answer, and `AppearancePreference::assumed` carries one bit per field so a
  fallback can never be read as a measurement (ADR-023 rule 7). `resolve_color_scheme` applies the
  documented light fallback and reports whether it resolved or assumed.
- `Window::appearance()` is the platform-neutral read. It returns the LAST measured snapshot, not a
  query: a platform that has a settings source keeps the value current and raises an event on a
  change, so the owner re-reads the accessor at that tick's boundary instead of polling. It is
  required to be cheap and non-blocking (an atomic load in the Linux implementation), so it is safe
  on the frame path.
- The change is the existing normalized path, not a bespoke callback: `WindowEventType::ThemeChanged`
  (new, mapping to the taxonomy's `ThemeChanged`) for a color-scheme change and the already-shipped
  `WindowEventType::SystemSettingsChanged` for the motion toggle. Both carry no value, exactly like
  the Win32 reduced-motion slice did, so no platform puts a preference inside a window event.
- ADR-020 holds: no D-Bus type, portal name or registry path appears in this header or in
  `Window::appearance()`. The portal decode is `include/tessera/hal/portal_appearance.hpp`, which
  names the freedesktop interface only in comments; the values it decodes are the enums above.

Platform halves, both of which only record:

- **Windows** (`src/platform/windows/window_appearance_win.cpp`): `SPI_GETCLIENTAREAANIMATION` for
  reduced motion (the same query the pre-existing `app::system_prefers_reduced_motion` used) and
  `HKCU\...\Themes\Personalize\AppsUseLightTheme` for the color scheme. `WM_SETTINGCHANGE` with the
  `ImmersiveColorSet` area and `SPI_SETCLIENTAREAANIMATION` classify to `ThemeChanged` /
  `SystemSettingsChanged` (`window_display_messages_win.cpp`).
- **Linux/X11** (`src/platform/linux/appearance_portal.{hpp,cpp}`): the freedesktop settings portal.
  One worker thread owns one D-Bus connection (the in-tree client in `src/dbus`, shared with the
  AT-SPI2 bridge), calls `org.freedesktop.portal.Settings.Read` for
  `org.freedesktop.appearance`/{`color-scheme`, `enable-animations`}, subscribes to
  `SettingChanged`, and publishes each new snapshot into one atomic word. `poll_events` turns the
  watcher's compare-and-clear into the normalized event. The **fallbacks** are: the portal, then
  GTK's `settings.ini` (`$XDG_CONFIG_HOME/gtk-3.0/settings.ini`, `gtk-enable-animations` and
  `gtk-application-prefer-dark-theme`), then the documented light/motion-on default with both
  assumptions set. A session with no bus, no portal or a refused call therefore keeps the default
  and emits no event, which is what the CI box and a bare X session get.
  `enable-animations` is **inverted**: the portal reports whether animations are allowed, the HAL
  reports whether motion should be reduced.

The app half is `AppHost::set_appearance` (`src/app/host_appearance.cpp`). The motion flag goes
through the existing `set_reduced_motion` path (animations, springs, the scheduler, scroll
clamping), and a change to **either** preference re-resolves the SAME media-independent prepared
sheet against the new `MediaContext` (ADR-048's resolve step), rebuilds the style generation and
drops the paint cache — the same path a theme switch takes. Keying the rebuild on the resolved media
context moving at all, rather than on the color scheme alone, is what makes the acceptance hold for a
sheet whose only media rule is `@media (prefers-reduced-motion: reduce)`: a motion-only change used to
move `reducedMotion` while leaving that rule's applicability at its startup value. A change that only
flips an assumption bit resolves to the same context and therefore costs no rebuild.
`ui::style::with_appearance` is the one place the HAL snapshot becomes CSS (`prefers-color-scheme`,
`prefers-reduced-motion`), and `AppHost::media_context` exposes the result.
`WindowHost::apply_appearance` is the tick-boundary re-read the events drive, and a settings
broadcast that moved nothing costs one comparison and no frame.

Tests: `hal_appearance` (the normalized model, the packed handoff, the portal value mapping
including the inversion, the `Read`/`SettingChanged` decoders built with the real `MessageWriter`
including a big-endian body, and the GTK parser), `app_host_appearance` (the acceptance: `AppHost`
resolves the media queries from the startup appearance, `set_appearance` moves `reducedMotion` and
the media evaluation — including the motion-only case above, asserted through the cascade's resolved
background — and the fake window's `ThemeChanged`/`SystemSettingsChanged` seam), and
`appearance_portal` (the absent-bus fallback and the real GTK path under `XDG_CONFIG_HOME`). All
three are deterministic with no bus, socket, clock or desktop, so they run identically on the
Windows and Linux gating lanes. Not exercised: a live portal on a real desktop.

Both Linux backends read the portal. It is session-wide, not an X11 facility, so
`src/platform/linux/window_wayland.cpp` runs the same `AppearancePortal` watcher and turns
`take_change()` into `ThemeChanged` at its poll boundary exactly as the X11 backend does; the TU is
therefore compiled whenever either backend is present, and a Wayland-only box kept the documented
fallback only because it had no source before.

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
| Window create/show | Win32 | Xlib/xcb | xdg-shell (implemented, #155) | Activity/native window | Cocoa |
| Resize/DPI | `WM_SIZE`, `WM_DPICHANGED` | `ConfigureNotify`, RandR | `xdg_toplevel.configure` + `ack` (implemented: compositor-owned size; scale stays 1.0 until wl_output/fractional-scale lands) | `onConfigurationChanged`, `WindowMetrics` | `NSWindowDelegate` |
| Display topology | DisplayConfig, `WM_DISPLAYCHANGE` | RandR | `wl_output`/`zxdg_output_v1` | `DisplayManager` | `NSScreen` |
| Refresh/VRR | DXGI/DisplayConfig | RandR/Present | `wl_output`, `wp_presentation` | `Display.Mode`, `Surface.setFrameRate` | `NSScreen.maximumFramesPerSecond`, CVDisplayLink |
| Power/thermal | power GUIDs, `WM_POWERBROADCAST` | logind/UPower | logind | `PowerManager` | `NSProcessInfo` |
| Fold/posture | no standard hinge API | no standard | compositor-specific | Jetpack WindowManager | no mainstream foldable |
| IME | `WM_IME_*` | XIM/IC | `zwp_text_input_v3` | `InputConnection` | `NSTextInputClient` |
| Clipboard (UTF-8 text) | Win32 `CF_UNICODETEXT` | unsupported stub (selection deferred) | unsupported stub | deferred | deferred |

Wayland rows are the #155 slice: window create/show/hide/close, the configure/ack handshake,
compositor-owned resize, activated/suspended lifecycle, and a `VK_KHR_wayland_surface` present
surface are implemented; input (`wl_seat`), text-input-v3/IME, display topology/refresh
(`wl_output`, `wp_presentation`), fractional scale, `xdg-decoration` and popups are follow-up
items. The Wayland backend is optional at build time (`pkg-config` `wayland-client` +
`wayland-protocols` + `wayland-scanner`) and selected per session: `WAYLAND_DISPLAY` picks Wayland,
`DISPLAY` alone keeps X11 exactly as before.

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
  `src/platform/linux/window.cpp` and the Wayland implementation in
  `src/platform/linux/window_wayland.cpp` + `window_wayland_shell.cpp` (generated xdg-shell client
  code). On Linux the selector in `src/platform/linux/window_linux.cpp` chooses per session:
  `WAYLAND_DISPLAY` set and the Wayland backend compiled in -> Wayland, otherwise X11 when
  `DISPLAY` is set, otherwise the stub. `TESSERA_WINDOW_BACKEND=x11|wayland` pins the backend for
  diagnostics and tests. Other platforms, and Linux configurations without X11 or Wayland, build a
  stub that returns `nullptr`. Window creation, normalized resize/DPI/focus/close events,
  owner-thread destruction, and the event-driven `wait_for_events` idle wait are covered by
  `window_win32`/`window_wait` on Windows, `window_x11` on Linux (skipped when `DISPLAY` is unset)
  and `window_wayland` (skipped with a diagnostic when `WAYLAND_DISPLAY` is unset or the
  compositor is unreachable). `include/tessera/hal/window_wait.hpp` supplies the deadline-bounded
  `wait_for_events_until` helper for the runtime/frame pacer. Wiring that helper into
  `RuntimeLoop` is still pending; the loop's tick behavior is unchanged.
- Wayland (issue #155): the backend owns the `wl_display` connection, binds `wl_compositor` and
  `xdg_wm_base` only (no `wl_seat`/`wl_output`/`wl_shm` this slice), answers the mandatory
  `xdg_wm_base.ping` and acks every `xdg_surface.configure` before a buffer commit, and reports the
  compositor-owned size through `WindowMetrics` (`dpi_scale` stays 1.0, `display_refresh_hz()`
  stays `kUnknownRefreshHz` - both documented "no source bound yet" answers). Normalized events:
  `Resize` from the toplevel configure, `Focus`/`Blur` from the `activated` state,
  `Occluded`/`Restored` from `suspended` (xdg-shell 6), and `Close` from `xdg_toplevel.close` or
  `Window::close()`. The presentation half is `VK_KHR_wayland_surface`: the swapchain builds
  `VkWaylandSurfaceCreateInfoKHR` from `native_handle()` (`wl_display*` + `wl_surface*`) and
  presents through the same bounded acquire/present path as X11. Deliberately not in this slice:
  input, text-input-v3/IME, fractional scale, decorations, popups and a Wayland full-screen
  request (`supports_fullscreen()` stays false; a compositor-initiated fullscreen is still reported
  through the metrics). Tests: `window_wayland` (window only, skips without a compositor) and
  `vulkan_swapchain_wayland` (window + Vulkan present, skips without a compositor/device/surface
  extension; verified headless under weston + lavapipe).
- Full-screen and the custom-caption seam (issue #232) are implemented on Win32 and X11.
  `include/tessera/hal/fullscreen.hpp` is the platform-neutral `FullscreenState` bookkeeping (a
  repeated request is a no-op, the pre-enter outer bounds are recorded once and consumed on exit, a
  mode the user changed outside the app is folded in), and every backend returns the resolved mode
  in `WindowMetrics::fullscreen` rather than the last request. The platform halves are
  `src/platform/windows/window_fullscreen_win.*` (borderless window over the current monitor,
  `WINDOWPLACEMENT` saved and reapplied) and `src/platform/linux/window_fullscreen_x11.*` (EWMH
  `_NET_WM_STATE_FULLSCREEN` via ClientMessage, mode read back from `_NET_WM_STATE`, unsupported when
  `_NET_SUPPORTED` does not advertise it). `include/tessera/hal/caption.hpp` carries the
  `CaptionRegionKind`/`CaptionRegion` contract the framework publishes through
  `Window::set_caption_regions` and the platform resolves to its OS hit-test answer
  (`src/platform/windows/window_caption_win.*`; X11 stores and reports the list but has no
  client-side decoration yet), with `WindowMetrics::caption_regions` reporting the same list and an
  empty list as the system-chrome default. Tests: `window_fullscreen`, `window_fullscreen_fake`,
  `window_win32`; contract in
  [`window-chrome-v1.md`](../specs/window-chrome-v1.md) section 8.4 and section 8.1.
- Pointer shapes (issue #191) are implemented on Win32 and X11; see section 1.5. `CursorShape` and
  `WindowMetrics::cursor` are the HAL surface, the platform halves are
  `src/platform/windows/window_cursor_win.*` (stock `IDC_*` cursors, `WM_SETCURSOR`) and
  `src/platform/linux/window_cursor_x11.*` (`XCreateFontCursor` cached per window, `XDefineCursor`),
  and the app's change-only request plus its one-shot diagnostic live in `src/app/window_cursor.*`.
  Tests: `window_cursor`, `window_cursor_fake`, `window_win32`, `window_x11`.
- Platform `VkSurfaceKHR`/swapchain paths live in `src/rendering/backend/vulkan/src/` and are
  created through `Device::create_swapchain` with the opaque `native_window` (HWND), or
  `native_display` + `native_window` (`Display*` + `Window` on X11, `wl_display*` + `wl_surface*` on
  Wayland) handles supplied by the HAL. `DeviceDesc::surface` (`rhi::SurfacePlatform`) is the
  additive RHI 1.4 field that tells the device which family the window belongs to: `Auto` resolves
  from the session environment exactly like `create_window()` (Wayland first, then X11), while an
  explicit value pins it; the window driver passes the HAL window's own family, so a Linux build
  with both backends never builds the wrong surface. Win32, Xlib and Wayland are implemented; other
  configurations report `NotSupported`. The Xlib smoke test runs under Xvfb with lavapipe and the
  Wayland one under headless weston with lavapipe.
- B1 is offscreen; the B3 window/swapchain path is Windows + Linux/X11 + Linux/Wayland. Remaining
  B3 slices are swapchain-image render targets, per-image frames-in-flight synchronization, live
  resize/device-loss policy, runtime/pacer integration, and the Wayland follow-ups listed above
  (input, IME, fractional scale/display data) (macOS via
  [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md); Android per
  [`mobile-integration.md`](mobile-integration.md)).
- X11 limitations in this slice: input is the **core protocol plus an X input method** (pointer,
  wheel and key events through `set_input_queue`, translated by `src/platform/linux/x11_input*.cpp`
  with the mapping rules in `x11_input_translation.cpp`; see [`input.md`](input.md)). Text comes
  from the window's XIC (`Xutf8LookupString`): dead keys, compose sequences and text committed by
  an input method (ibus, fcitx) arrive as `Text` events, and a dead key is still a key event that
  types nothing. Every event goes through `XFilterEvent` first; with no input-method server the
  built-in `@im=none` method still composes. A window left at the `C` locale gets `C.UTF-8` for
  `LC_CTYPE` only. `MappingNotify` refreshes Xlib's keymap, so a layout switch applies to a
  running app. XInput2 (smooth scroll, touch, pen) and client-side pre-edit
  (`XIMPreeditCallbacks`) are translated in `x11_xi2*.cpp`; `Window::set_text_input_rect` anchors the
  candidate window via `XNSpotLocation` (`x11_ime_spot.*`, the app does not call it yet). Only the built-in
  method is tested, not a real ibus or fcitx server. Tests: `window_x11_input` sends
  synthetic events with `XSendEvent`; `window_x11_real_input` drives the window through the XTEST
  extension (what `xdotool` uses) so the X server itself generates the pointer, wheel, key and
  focus events (verified under Xvfb on Linux; it skips without `DISPLAY` or libXtst). There is
  no occlusion/minimize signal, no RandR
  or display-hotplug handling, and DPI is a global `Xft.dpi` heuristic (96 fallback). Wayland is a
  separate backend (issue #155). See [`window-resize.md`](window-resize.md) §0.
- Mobile/foldable support follows [`os-events-v1.md`](../specs/os-events-v1.md).
