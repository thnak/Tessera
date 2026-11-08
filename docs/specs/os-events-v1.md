# OS events and display capabilities v1

Status: proposed for Big Step B3 (window/swapchain path with resize/DPI/lifecycle, defined in
[AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big Steps"). Researched against platform documentation
(see §12). Decision:
[ADR-023](../../decisions/ADR-023-os-event-normalization-and-foldables.md) (OS event
normalization and foldables). Normative companion to
[`window-resize.md`](../design/window-resize.md). Mobile
surface/input/IME/accessibility/declarations are in
[`mobile-integration.md`](../design/mobile-integration.md).

## 1. Purpose and rules

Tessera normalizes OS events into a small, platform-neutral event model. The HAL (hardware/OS
abstraction layer, [`hal.md`](../design/hal.md)) owns the platform APIs; the declarative layer
(styles/animations/components) only sees `WindowMetrics` (normalized window/display/fold
metrics, defined in [`window-resize.md`](../design/window-resize.md)), display capabilities, and
normalized input/lifecycle events. Abbreviations used below: IME = input method editor; PiP =
picture-in-picture; VRR = variable refresh rate; HDR = high dynamic range.

Rules:

1. A platform callback **records** an event; it does not lay out, render, or block.
2. Events are coalesced and consumed at a frame/tick boundary (a tick is one fixed-step
   update; see [`game-loop.md`](../design/game-loop.md)).
3. Ordering is stable and documented; synthetic event scripts are deterministic.
4. Events that can change visible output are wake sources for the loop (they request a frame
   in render-on-demand mode; see the [glossary](../reference/glossary.md)).
5. Unknown/unsupported events are ignored with a diagnostic; never a crash.
6. Foldables are modeled as **display features + metrics**, not as a separate UI mode.
7. No UI-thread blocking on the compositor/GPU; resize/swapchain follow
   [`window-resize.md`](../design/window-resize.md).

## 2. Normalized event model

```cpp
enum class OsEventType {
    WindowCreated, WindowShown, WindowHidden, WindowClosed,
    WindowActivated, WindowDeactivated, WindowFocused, WindowBlurred,
    WindowResized, WindowMoved, WindowStateChanged,
    DpiChanged, ScaleChanged, OrientationChanged,
    DisplayAdded, DisplayRemoved, DisplayChanged, DisplayModeChanged,
    RefreshRateChanged, VrrChanged, HdrChanged, ColorGamutChanged,
    SafeAreaChanged, WorkAreaChanged,
    FoldChanged, PostureChanged, MultiWindowChanged, PictureInPictureChanged,
    InputDeviceChanged, ImeChanged,
    ThemeChanged, AccentColorChanged, HighContrastChanged, ReducedMotionChanged,
    TextScaleChanged, LocaleChanged,
    PowerSourceChanged, BatterySaverChanged, ThermalStateChanged,
    Suspend, Resume, ScreenOff, ScreenOn, SessionLocked, SessionUnlocked,
    MemoryPressure, ScreenCaptureChanged, ShutdownRequested
};

struct OsEvent {
    OsEventType type;
    std::uint64_t sequence;
    std::int64_t timestamp_ns;       // monotonic; simulated in deterministic mode
    WindowId window;
    DisplayId display;
    WindowMetrics metrics;           // see window-resize.md
    FoldInfo fold;                   // hinge/fold, if any
    EventReason reason;              // user, dpi, display, maximize, restore, ...
    bool handled;
};
```

In the platform tables below, a shorthand such as `Theme/Accent/HighContrast` abbreviates
`ThemeChanged/AccentColorChanged/HighContrastChanged`, and `Occluded` is not a value of
`OsEventType` (occlusion is reported through `WindowMetrics` and `WindowHidden`; the file does
not define a separate event).

`WindowMetrics` carries logical/physical size, DPI/scale, refresh/VRR, safe/work areas,
orientation, focus/occlusion/minimized/fullscreen, and multi-window/PiP state.

```cpp
enum class FoldState { None, Flat, HalfOpened };
enum class FoldOrientation { None, Horizontal, Vertical };
enum class FoldOcclusion { None, Full };

struct FoldInfo {
    FoldState state = FoldState::None;
    FoldOrientation orientation = FoldOrientation::None;
    FoldOcclusion occlusion = FoldOcclusion::None;
    bool separating = false;
    Rect hinge_bounds;             // logical pixels, window coordinates
};
```

## 3. Event ordering and delivery

Per frame/tick:

```text
platform callbacks -> raw queue
  -> normalize (window/display/fold/input/power)
  -> coalesce by (window, type, display)
  -> fixed order:
       window lifecycle -> display/DPI/orientation -> fold/posture
       -> input -> style/layout/paint -> render
  -> apply UI-thread mutations -> mark dirty
```

- **Coalesce:** resize, move, DPI, refresh, pointer-move; keep the latest value and union of
  reasons. Never drop lifecycle/fold/power transitions.
- **Sequence:** monotonically increasing per window/display; stale sequences are discarded.
- **Reentrancy:** no user code inside the platform callback; no nested layout/render.
- **Wake:** visible changes request a frame; hidden/occluded changes may only update state.
- **Determinism:** recorded scripts inject the same `OsEvent` sequence; platform APIs are not
  called in golden mode.

## 4. Windows

Representative messages/APIs (not exhaustive):

| Trigger | Source | Normalized | Runtime action |
|---|---|---|---|
| Window created/shown/hidden | `WM_CREATE`, `WM_SHOWWINDOW` | `WindowCreated/Shown/Hidden` | create/pause surfaces |
| Move/size | `WM_MOVE`, `WM_SIZE`, `WM_WINDOWPOSCHANGED` | `WindowMoved/Resized` | metrics, layout, target recreate |
| Interactive resize start/end | `WM_ENTERSIZEMOVE`, `WM_EXITSIZEMOVE` | `WindowResized` (reason=live) | coalesce during modal loop; recreate on exit |
| Min/max constraints | `WM_GETMINMAXINFO` | `WindowStateChanged` | clamp window bounds |
| DPI change | `WM_DPICHANGED` | `DpiChanged/ScaleChanged` | apply suggested rect; style/layout/targets |
| Display resolution change | `WM_DISPLAYCHANGE` | `DisplayModeChanged` | refresh/mode query, target recreate |
| Work area/theme/settings | `WM_SETTINGCHANGE`, `WM_THEMECHANGED`, `WM_SYSCOLORCHANGE` | `WorkArea/Theme/Accent/HighContrast` | layout/theme invalidation |
| App/window activation | `WM_ACTIVATEAPP`, `WM_ACTIVATE`, `WM_NCACTIVATE` | `WindowActivated/Deactivated` | focus/pacing/pseudo-state |
| Monitor/device add/remove | `WM_DEVICECHANGE`, `RegisterDeviceNotification(GUID_DEVINTERFACE_MONITOR)`, DisplayConfig | `DisplayAdded/Removed/Changed` | display list, refresh/VRR, target recreate |
| Power source/battery saver | `WM_POWERBROADCAST`, `RegisterPowerSettingNotification`, `GUID_ACDC_POWER_SOURCE`, `GUID_POWER_SAVING_STATUS` | `PowerSourceChanged/BatterySaverChanged` | power policy, pacing, adaptive quality |
| Display on/off/dim | `GUID_CONSOLE_DISPLAY_STATE`, `GUID_SESSION_DISPLAY_STATUS`, `GUID_MONITOR_POWER_ON` | `ScreenOn/ScreenOff` | pause/resume rendering |
| Suspend/resume | `WM_POWERBROADCAST` (`PBT_APMSUSPEND`, `PBT_APMRESUMEAUTOMATIC`) | `Suspend/Resume` | pause/restore, invalidate resources |
| Session lock/unlock | `WTSRegisterSessionNotification`, `WM_WTSSESSION_CHANGE` | `SessionLocked/Unlocked` | pause/power policy |
| Shutdown/logoff | `WM_QUERYENDSESSION`, `WM_ENDSESSION` | `ShutdownRequested` | cancel/cleanup/async shutdown |
| Theme/high contrast/reduced motion | `WM_SETTINGCHANGE`, `WM_THEMECHANGED`, `UISettings` (WinRT) | `Theme/HighContrast/ReducedMotion` | style re-evaluate |
| Text scale/locale | `WM_SETTINGCHANGE`, `WM_INPUTLANGCHANGE` | `TextScaleChanged/LocaleChanged` | text/layout |
| Pointer/pen/touch | `WM_POINTER*`, `WM_TOUCH`, `WM_GESTURE` | `InputDeviceChanged` + pointer events | input dispatch |
| Keyboard/IME | `WM_KEY*`, `WM_CHAR`, `WM_UNICHAR`, `WM_IME_*` | `ImeChanged` + key/text | focus/text input |
| Clipboard/drag-drop | `WM_CLIPBOARDUPDATE`, `WM_DROPFILES`, OLE drop | `InputDeviceChanged`/payload | UI actions |
| Raw input | `WM_INPUT` + `GetRawInputData` | raw pointer/key | input dispatch |

Compliance notes:

- Per-monitor DPI awareness v2; honor `WM_DPICHANGED` suggested rect.
- Do not fight the modal resize loop; coalesce and recreate on `WM_EXITSIZEMOVE` (or another
  point where no GPU frame is in flight; the exact boundary is defined in
  [`window-resize.md`](../design/window-resize.md)).
- Re-query display topology/refresh/VRR after `WM_DISPLAYCHANGE`/device notifications.
- `WM_SETTINGCHANGE` covers many accessibility/theme/work-area changes; inspect the parameter.
- Never block the message loop with GPU/device waits.

## 5. Linux / X11

Representative events (not exhaustive):

| Trigger | Source | Normalized | Runtime action |
|---|---|---|---|
| Resize/move | `ConfigureNotify`, `ConfigureRequest` | `WindowResized/Moved` | metrics/layout/target |
| Map/unmap/destroy | `MapNotify`, `UnmapNotify`, `DestroyNotify` | `WindowShown/Hidden/Closed` | visibility/pacing |
| Focus/enter/leave | `FocusIn/Out`, `EnterNotify/LeaveNotify` | `WindowFocused/Blurred`, hover | input/style |
| State/property changes | `PropertyNotify`, `_NET_WM_STATE`, `WM_NORMAL_HINTS` | `WindowStateChanged`, constraints | maximize/fullscreen/tiled/min |
| Close request | `ClientMessage` (`WM_DELETE_WINDOW`) | `WindowClosed` | graceful shutdown |
| Display/CRTC/output changes | RandR `RRScreenChangeNotify`, `RRNotify_OutputChange/CrtcChange`, `XRRUpdateConfiguration` | `DisplayAdded/Removed/Changed`, `RefreshRateChanged` | re-query modes/DPI, target recreate |
| Output/DPI/scale | `XRRGetMonitors`, `XRRGetOutputInfo`, `XRRGetCrtcInfo`, `XRRGetOutputPrimary` | `DpiChanged/ScaleChanged/SafeArea` | style/layout |
| Resize synchronization | `_NET_WM_SYNC_REQUEST`, `_NET_WM_SYNC_REQUEST_COUNTER` | `WindowResized` (sync) | throttle, no blocking |
| Ping/liveness | `_NET_WM_PING`, `WM_PROTOCOLS` | health/diagnostic | respond, no freeze |
| Raw/pointer/touch/gesture | XInput2 `XI_RawMotion`, `XI_RawButton*`, `XI_TouchBegin/Update/End`, `XI_Gesture*` | input events | dispatch |
| Keyboard/layout | `KeyPress/Release`, `XkbStateNotify`, `XkbMapNotify` | key/text, `LocaleChanged` | focus/shortcuts |
| Present timing | Present `PresentCompleteNotify`, `PresentIdleNotify`, `PresentConfigureNotify` | frame presented/idle | pacing/diagnostics |
| Clipboard | XFixes `SelectionNotify`/selection events | clipboard changed | UI actions |
| Drag-and-drop | XDND (`XdndEnter`, `Position`, `Status`, `Leave`, `Drop`, `Finished`) | drag events | UI actions |
| Session/power | D-Bus `org.freedesktop.login1` `PrepareForSleep`, `Lock/Unlock`; UPower `PropertiesChanged` | `Suspend/Resume/SessionLocked/Unlocked/PowerSource` | power policy |
| Theme/accent/reduced motion | XDG desktop portal `org.freedesktop.appearance`, settings changes | `Theme/Accent/ReducedMotion` | style re-evaluate |

Compliance notes:

- Handle `RRScreenChangeNotify` and call `XRRUpdateConfiguration` before reading RandR state.
- Use the sync counter only as a hint; never block the UI thread waiting for it.
- Re-query refresh/scale on output changes; multi-monitor is per-output.
- Use Present feedback for pacing; do not busy-wait on the X server.

## 6. Linux / Wayland

Representative protocol events (not exhaustive):

| Trigger | Source | Normalized | Runtime action |
|---|---|---|---|
| Surface configure | `xdg_surface.configure` + `ack_configure` | `WindowResized/StateChanged` | apply last configure, recreate target |
| Toplevel size/state | `xdg_toplevel.configure` (`width`, `height`, states) | `WindowResized/StateChanged` | max/fullscreen/tiled/resizing/activated |
| Bounds/capabilities | `xdg_toplevel.configure_bounds`, `wm_capabilities` | constraints/capabilities | clamp/disable UI actions |
| Close request | `xdg_toplevel.close` | `WindowClosed` | graceful shutdown |
| Suspended/occluded | `xdg_toplevel` state `suspended`, frame callbacks | `WindowHidden/Occluded` | pause rendering |
| Popup lifecycle | `xdg_popup.configure`, `popup_done`, `repositioned` | menu/popover events | UI state |
| Output add/remove/change | `wl_registry.global/global_remove` for `wl_output` | `DisplayAdded/Removed/Changed` | display list |
| Output mode/scale | `wl_output.geometry/mode/scale/name/description/done`, `zxdg_output_v1` | `RefreshRateChanged/ScaleChanged` | style/layout/pacing |
| Surface output/scale | `wl_surface.enter/leave`, `preferred_buffer_scale`, `wp_fractional_scale_v1` | `ScaleChanged/SafeArea` | raster scale/viewport |
| Frame timing | `wl_surface.frame`, `wp_presentation` feedback | frame presented/idle | pacing/diagnostics |
| Seat capabilities/input | `wl_seat.capabilities`, `wl_pointer`, `wl_keyboard`, `wl_touch` | input events | dispatch |
| Text input/IME | `zwp_text_input_v3`, `zwp_input_method_v2` | `ImeChanged` + text | focus/text |
| Decorations | `zxdg_toplevel_decoration_v1.configure` | decoration mode | frame UI |
| Session lock | `ext_session_lock_v1` | `SessionLocked/Unlocked` | pause/security |
| Idle inhibit | `zwp_idle_inhibit_v1` | power/diagnostic | keep-awake |
| Tearing/content type | `wp_tearing_control_v1`, `wp_content_type_v1` | present policy | pacing/quality |

Compliance notes:

- **Never draw before `ack_configure`** for the serial you use; ack the last configure and
  discard stale buffers.
- The compositor owns the size; `width/height` may be zero and states may change at any time.
- `set_minimized` has no query result; use `suspended` state/frame callbacks for throttling.
- Use `wl_surface.frame` and `wp_presentation` for pacing; do not busy-wait.
- Fractional scale: `wp_fractional_scale_v1` + `wp_viewporter`; integer scale:
  `wl_surface.set_buffer_scale`.
- No standard foldable/hinge protocol; treat hardware as output/mode/scale changes and
  compositor-specific protocols.

## 7. Android

Representative callbacks/APIs (not exhaustive):

| Trigger | Source | Normalized | Runtime action |
|---|---|---|---|
| Configuration change | `Activity.onConfigurationChanged` / `Configuration` | `WindowResized/Orientation/Dpi/TextScale/Locale/Theme` | style/layout/text |
| Orientation | `Configuration.orientation`, `Display.getRotation` | `OrientationChanged` | layout/recreate |
| Screen size/density | `screenWidthDp`, `screenHeightDp`, `smallestScreenWidthDp`, `densityDpi`, `screenLayout` | `WindowResized/DpiChanged` | layout/style |
| Night/theme | `uiMode`, `colorMode` | `ThemeChanged/HdrChanged/ColorGamutChanged` | style/paint |
| Font scale/weight | `fontScale`, `fontWeightAdjustment` | `TextScaleChanged` | text/layout |
| Locale/layout direction | `LocaleList`, `layoutDirection` | `LocaleChanged` | text/layout |
| Multi-window | `onMultiWindowModeChanged`, `isInMultiWindowMode` | `MultiWindowChanged` | layout/pacing |
| Picture-in-Picture | `onPictureInPictureModeChanged` | `PictureInPictureChanged` | layout/pacing |
| Window insets | `WindowInsets` (`statusBars`, `navigationBars`, `systemBars`, `displayCutout`, `ime`, `systemGestures`, `tappableElement`, `mandatorySystemGestures`) | `SafeAreaChanged/ImeChanged` | layout/IME |
| Insets animation | `WindowInsetsAnimation.Callback` | `SafeAreaChanged/ImeChanged` | animate layout |
| Display add/remove/change | `DisplayManager.DisplayListener` (`onDisplayAdded/Removed/Changed`) | `DisplayAdded/Removed/Changed` | display list |
| Refresh rate | `Display.getRefreshRate`, `Display.Mode`, `preferredDisplayModeId`, `Surface.setFrameRate`, `EVENT_TYPE_DISPLAY_REFRESH_RATE` (API 36+) | `RefreshRateChanged/VrrChanged` | pacing/content rate |
| Display HDR/color | `Display.HdrCapabilities`, `Configuration.colorMode` | `HdrChanged/ColorGamutChanged` | paint/quality |
| Fold state/posture | Jetpack WindowManager `WindowInfoTracker` + `FoldingFeature` | `FoldChanged/PostureChanged` | two-pane/posture layout |
| Fold/hinge bounds | `FoldingFeature.getBounds`, `getOrientation`, `getOcclusionType`, `isSeparating` | `FoldInfo` | avoid hinge/occlusion |
| App continuity | Activity stop/restart on fold/unfold; `onSaveInstanceState` | lifecycle + `FoldChanged` | restore state |
| Focus/activation | `onWindowFocusChanged`, `onTopResumedActivityChanged` | `WindowFocused/Blurred` | input/pacing |
| Keyboard/back | `KeyEvent`, `OnBackInvokedCallback` | input events | dispatch |
| Touch/stylus/mouse | `MotionEvent` (`ACTION_*`, `TOOL_TYPE_*`, axes) | pointer/stylus events | dispatch |
| IME/text | `InputConnection`, `EditorInfo`, `WindowInsets.Type.ime()` | `ImeChanged` + text | focus/text |
| Power/thermal | `PowerManager` (`isPowerSaveMode`, `getCurrentThermalStatus`, `OnThermalStatusChangedListener`), battery broadcasts | `PowerSource/BatterySaver/ThermalState` | power policy |
| Screen/user presence | `ACTION_SCREEN_ON/OFF`, `ACTION_USER_PRESENT` | `ScreenOn/Off` | pause/resume |
| Memory pressure | `onTrimMemory`, `onLowMemory` | `MemoryPressure` | evict caches |
| Display capture | `Display`/window flags, `FLAG_SECURE`, screen capture state | `ScreenCaptureChanged` | privacy/quality |

Foldable semantics (Android):

- `FoldingFeature.State`: `FLAT` (fully open) or `HALF_OPENED`.
- `FoldingFeature.Orientation`: `HORIZONTAL` (tabletop posture) or `VERTICAL` (book posture).
- `FoldingFeature.OcclusionType`: `NONE` (guideline/fold) or `FULL` (hinge/occluded area).
- `isSeparating`: true when the feature splits the window into two logical areas.
- `bounds` are in window coordinates; avoid placing controls under `FULL` occlusion.
- The app may stop/restart when folding/unfolding; preserve/restore state for continuity.
- Samsung calls the half-opened posture "Flex mode"; use the same Jetpack APIs.

## 8. iOS / iPadOS

Representative delegates/notifications (not exhaustive; the iOS backend is not implemented;
[AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step) Big Step F says "Android first; iOS later"):

| Trigger | Source | Normalized | Runtime action |
|---|---|---|---|
| Scene connect/disconnect | `UISceneDelegate.scene(_:willConnectTo:options:)`, `sceneDidDisconnect` | `WindowCreated/Closed` | setup/teardown |
| Foreground/background | `sceneWillEnterForeground`, `sceneDidEnterBackground` | `WindowShown/Hidden` | pause/resume |
| Active/inactive | `sceneDidBecomeActive`, `sceneWillResignActive` | `WindowActivated/Deactivated` | focus/pacing |
| Scene geometry/orientation | `UIWindowSceneDelegate` geometry updates (iOS 16+), `viewWillTransition(to:with:)` | `WindowResized/OrientationChanged` | layout/target |
| Trait changes | `traitCollectionDidChange` / `registerForTraitChanges` (`displayScale`, size classes, style, gamut, contrast) | `DpiChanged/Theme/HighContrast/ColorGamut` | style/layout/paint |
| Content size category | `UIContentSizeCategory.didChangeNotification` | `TextScaleChanged` | text/layout |
| Reduce motion | `UIAccessibility.reduceMotionStatusDidChangeNotification` | `ReducedMotionChanged` | animation policy |
| Bold/invert/VoiceOver/Switch | `UIAccessibility` notifications | accessibility state | style/behavior |
| Display connect/disconnect | `UIScreen.didConnectNotification`, `didDisconnectNotification` | `DisplayAdded/Removed` | display list |
| Display mode/refresh | `UIScreen.mode`, `availableModes`, `maximumFramesPerSecond` | `DisplayModeChanged/RefreshRateChanged` | pacing/quality |
| Screen capture | `UIScreen.isCaptured` | `ScreenCaptureChanged` | privacy/quality |
| Memory warning | `didReceiveMemoryWarningNotification` | `MemoryPressure` | evict caches |
| Screenshot | `userDidTakeScreenshotNotification` | diagnostic | overlay/report |
| Multitasking/Stage Manager | `UIWindowScene.session.role`, `activationState`, `UISceneSession` | `MultiWindowChanged` | layout/pacing |
| External display | `UISceneSession.Role.windowExternalDisplay*` | `DisplayAdded/Removed` | separate scene |
| Low power/thermal | `NSProcessInfoPowerStateDidChangeNotification`, `thermalStateDidChangeNotification` | `BatterySaver/ThermalState` | power policy |

Compliance notes:

- Use scene lifecycle, not the legacy app delegate, for multi-window/iPadOS.
- iPad multitasking/Stage Manager/external displays are separate scenes; do not assume one window.
- Use safe-area insets and trait collections; never hardcode screen bounds.
- No mainstream foldable iOS hardware; if it appears, model it through scene geometry/traits.

## 9. macOS (deferred, ADR-010)

macOS support is deferred from V1 (first-release scope) by
[ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md) (macOS through MoltenVK).

Representative delegates/notifications (not exhaustive):

| Trigger | Source | Normalized | Runtime action |
|---|---|---|---|
| App launch/terminate | `NSApplicationDelegate` | `WindowCreated/ShutdownRequested` | setup/teardown |
| App active/inactive/hide | `applicationDidBecomeActive`, `applicationDidResignActive`, `applicationDidHide/Unhide` | `WindowActivated/Deactivated/Hidden` | focus/pacing |
| Resize/move | `windowDidResize`, `windowDidMove`, `windowWillStartLiveResize`, `windowDidEndLiveResize` | `WindowResized/Moved` | layout/target |
| Backing scale | `windowDidChangeBackingProperties`, `viewDidChangeBackingProperties` | `DpiChanged/ScaleChanged` | style/layout/target |
| Screen change | `windowDidChangeScreen`, `NSApplication.didChangeScreenParametersNotification` | `DisplayChanged` | refresh/scale/target |
| Occlusion | `windowDidChangeOcclusionState` | `WindowHidden/Occluded` | pause/resume |
| Miniaturize/fullscreen | `windowDidMiniaturize/Deminiaturize`, `windowDidEnter/ExitFullScreen` | `WindowStateChanged` | layout/pacing |
| Theme/appearance | `NSApp.effectiveAppearance`, `NSWorkspace.accessibilityDisplayOptionsDidChangeNotification` | `Theme/Accent/HighContrast/ReducedMotion` | style |
| Low power/thermal | `NSProcessInfoPowerStateDidChangeNotification`, `thermalStateDidChangeNotification` | `BatterySaver/ThermalState` | power policy |
| Sleep/wake | `NSWorkspace.willSleepNotification`, `didWakeNotification`, `screensDidSleep/Wake` | `Suspend/Resume/ScreenOn/Off` | pause/restore |
| Screen refresh | `NSScreen.maximumFramesPerSecond`, `CVDisplayLink`/`CAMetalDisplayLink` | `RefreshRateChanged` | pacing |
| Safe area | `NSScreen.safeAreaInsets`, `NSWindow.contentLayoutRect` | `SafeAreaChanged` | layout |
| Input | `NSEvent` (mouse/keyboard/trackpad/gesture) | input events | dispatch |

## 10. Foldables and multi-form-factor matrix

| Form factor | Key signals | Normalized | UI obligation |
|---|---|---|---|
| Desktop | multi-monitor, DPI, refresh/VRR, HDR, work area | `Display*`, `Dpi/Scale`, `Refresh/Vrr`, `Hdr/ColorGamut` | per-monitor layout, no hardcoded 60 Hz/1x |
| Mobile | orientation, insets, cutout, IME, multi-window/PiP | `Orientation/Resize/SafeArea/Ime/MultiWindow/PiP` | safe-area-aware, resize without restart |
| Foldable | fold state, orientation, occlusion, separating, hinge bounds | `FoldChanged/PostureChanged` + `FoldInfo` | two-pane/tabletop/book layout, avoid hinge, continuity |
| Dual-screen | hinge occlusion `FULL`, separating | `FoldInfo{occlusion=Full,separating=true}` | never place content under hinge |
| Tablet/iPad | size classes, Stage Manager, external display | `MultiWindow/Display*` | multi-scene, adaptive layout |
| Desktop foldable PC | display topology + DPI/orientation changes (no standard hinge API) | `Display*/Dpi/Resize` | verify per device; no vendor assumption |
| Linux foldable | no standard protocol; compositor-specific | best-effort `Display/Scale` | degrade to size/scale changes |

Fold layout rules:

- `HALF_OPENED` + horizontal fold -> tabletop: top content, bottom controls.
- `HALF_OPENED` + vertical fold -> book: left/right logical panes.
- `occlusion=FULL` or `separating=true` -> never place interactive content in the hinge area.
- `FLAT` -> treat as a normal large screen; avoid unnecessary two-pane mode.
- Preserve selection/scroll/focus across fold/unfold (app continuity).
- Do not restart animations on every fold transition; retarget at a frame boundary.

## 11. Runtime mapping

In this table, "maybe" means the effect depends on whether the change touches that domain; the
condition is not specified here, and "yes"/"no" mean always/never.

| Normalized event | Style environment | Layout | Paint/render | Pacing/power | Diagnostics |
|---|---|---|---|---|---|
| `WindowResized/Moved` | viewport units | root/subtree | target recreate | suspend/resume pacing | resize metrics |
| `DpiChanged/ScaleChanged` | media queries/rem | yes | raster scale/target | refresh re-query | dpi metric |
| `OrientationChanged` | media queries | yes | yes | yes | orientation |
| `Display*`/`RefreshRateChanged` | resolution queries | yes | target/format | refresh/VRR renegotiate | display list |
| `FoldChanged/PostureChanged` | container/size queries | two-pane/tabletop | yes | maybe pause | fold metrics |
| `SafeAreaChanged` | padding tokens | yes | yes | no | insets |
| `Theme/Accent/HighContrast` | custom properties | maybe | repaint | no | theme |
| `ReducedMotion` | animation policy | maybe | no | no | a11y |
| `TextScale/Locale` | font/text | text reflow | glyph cache | no | text metrics |
| `PowerSource/BatterySaver/Thermal` | no | no | quality | policy/adaptive | power/thermal |
| `Suspend/Resume/ScreenOff/On` | no | no | pause/resume | yes | lifecycle |
| `SessionLocked/Unlocked` | no | no | pause/resume | yes | security |
| `MemoryPressure` | no | no | evict caches | no | memory |
| `ScreenCaptureChanged` | no | no | privacy/quality | no | security |
| `ShutdownRequested` | no | no | stop presenting | cancel tasks | shutdown |

## 12. Testing and references

Testing:

- Record/replay synthetic `OsEvent` scripts per platform.
- Resize/DPI storms, multi-monitor moves, output hotplug, refresh/VRR changes.
- Fold sequences: flat -> half-opened (tabletop/book) -> flat, including separating/full
  occlusion and app continuity.
- Suspend/resume, session lock/unlock, screen off/on, battery saver/thermal transitions.
- Display cutout/safe area/IME insets, orientation changes, multi-window/PiP.
- Golden mode: no platform callbacks; deterministic event sequence only.

Research references (official documentation):

- Android foldables: [Learn about foldables](https://developer.android.com/develop/adaptive-apps/guides/foldables/learn-about-foldables),
  [Make your app fold aware](https://developer.android.com/develop/adaptive-apps/guides/foldables/make-your-app-fold-aware),
  [FoldingFeature](https://developer.android.com/reference/androidx/window/layout/FoldingFeature),
  [Configuration](https://developer.android.com/reference/android/content/res/Configuration),
  [DisplayManager](https://developer.android.com/reference/android/hardware/display/DisplayManager),
  [Handle configuration changes](https://developer.android.com/guide/topics/resources/runtime-changes),
  [Display.Mode](https://developer.android.com/reference/android/view/Display.Mode),
  [Surface.setFrameRate](https://developer.android.com/reference/android/view/Surface#setFrameRate(float,int)),
  [WindowMetrics](https://developer.android.com/reference/android/view/WindowMetrics),
  [WindowInsets](https://developer.android.com/reference/android/view/WindowInsets),
  [PowerManager thermal status](https://developer.android.com/reference/android/os/PowerManager#getCurrentThermalStatus()).
- Samsung Flex mode: [Flex mode](https://developer.samsung.com/galaxy-z/flex-mode.html).
- Windows: [WM_ENTERSIZEMOVE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-entersizemove),
  [WM_EXITSIZEMOVE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-exitsizemove),
  [WM_SIZE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-size),
  [WM_WINDOWPOSCHANGED](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-windowposchanged),
  [WM_DPICHANGED](https://learn.microsoft.com/en-us/windows/win32/hidpi/wm-dpichanged),
  [WM_DISPLAYCHANGE](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-displaychange),
  [WM_SETTINGCHANGE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-settingchange),
  [Power Setting GUIDs](https://learn.microsoft.com/en-us/windows/win32/power/power-setting-guids).
- Wayland: [xdg-shell](https://wayland.app/protocols/xdg-shell),
  [presentation-time](https://wayland.app/protocols/presentation-time),
  [fractional-scale-v1](https://wayland.app/protocols/fractional-scale-v1),
  [Wayland protocol](https://wayland.app/protocols/wayland).
- X11: [Xrandr](https://xorg.freedesktop.org/archive/X11R7.6/doc/man/man3/Xrandr.3.xhtml),
  [X11 multitouch](https://www.x.org/Development/Documentation/Multitouch/),
  [X.Org documentation](https://www.x.org/Documentation/).
- iOS/iPadOS: [UISceneDelegate](https://developer.apple.com/documentation/uikit/uiscenedelegate/),
  [UIWindowSceneDelegate](https://developer.apple.com/documentation/uikit/uiwindowscenedelegate),
  [Preparing your UI to run in the foreground](https://developer.apple.com/documentation/uikit/preparing-your-ui-to-run-in-the-foreground).
- macOS: [NSWindowDelegate](https://developer.apple.com/documentation/appkit/nswindowdelegate),
  [NSApplicationDelegate](https://developer.apple.com/documentation/appkit/nsapplicationdelegate).

## 13. Open questions

- Windows foldable/dual-screen: no stable cross-vendor hinge API; per-device verification.
- Android multi-display topology and desktop windowing: which signals are stable across OEMs.
- Wayland compositor-specific fold/hinge protocols (none known; no decision yet) and fallback
  behavior.
- iOS external display/Stage Manager scene roles across versions.
- Standardized thermal/power taxonomy across platforms.
- Whether to expose raw platform events for advanced integrations behind a capability flag.
