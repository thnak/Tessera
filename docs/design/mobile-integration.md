# Mobile integration: OS event fidelity, surfaces, and declarations

Status: design only. No Android/iOS backend is implemented; B1 is desktop/offscreen.
Android is the first mobile target; iOS/iPadOS is deferred (ADR-010 covers macOS, not iOS).

Tessera owns the renderer and input pipeline, but **the OS owns the app lifecycle, surface,
window/view hierarchy, input delivery, IME, accessibility, and permissions**. The HAL must be a
thin, faithful bridge between those OS contracts and Tessera's normalized events.

## 1. The core problem

On desktop, a window is mostly "a surface plus messages". On mobile, the OS sees **one native
view/surface**, not Tessera's component tree. The OS will not automatically provide:

- accessibility for custom-drawn components (TalkBack/VoiceOver see one view)
- text input/IME for custom-drawn text fields
- focus/keyboard/pointer/stylus/gamepad semantics
- selection, copy/paste, autofill, password managers, spellcheck, dictation
- system back/navigation gestures
- safe-area/cutout/IME insets and fold/multi-window adaptation
- lifecycle/surface-loss handling, background restrictions, power/thermal policy
- permissions, privacy manifests, and store declarations

Therefore mobile support is not just "create a Vulkan surface". It is a **platform bridge**
with four contracts:

```text
Surface/lifecycle contract  -> when can we render, and to what?
Input contract              -> what happened, with full fidelity?
Semantics/IME contract      -> what does the OS/accessibility see?
App declaration contract    -> what did we declare/request, and what is allowed?
```

## 2. Event fidelity contract

The platform event is the source of truth. Normalize it, do not lose information.

Preserve in normalized events:

- timestamp (platform monotonic clock where available)
- source/device id and source class (touch, mouse, stylus, keyboard, gamepad, trackpad)
- pointer id, tool type, button/buttons
- pressure, tilt, orientation, contact size where available
- historical/coalesced samples (Android `MotionEvent` history, iOS coalesced touches)
- keycode, unicode/text, modifiers, repeat, canceled flag
- IME composition text/selection and commit/cancel
- window/display/fold/config generation at the time of the event

Rules:

- Do not synthesize a tap from raw touch if it loses multi-touch/gesture semantics.
- Define precedence between platform gesture recognizers and Tessera handlers explicitly.
- Delivery is queued and consumed at a tick/frame boundary; no layout/render/user code inside
  a platform callback.
- Coalesce move/scroll/resize/refresh; never drop lifecycle, fold, cancel, focus, or IME
  transitions.
- **Cancellation is a first-class event**: Android `ACTION_CANCEL`, iOS `touchesCancelled`,
  pointer-capture loss, focus loss, app background, surface destroy, window resize/fold.
- Cancel gestures and release captures on background/surface loss; no stale active state.
- Record/replay normalized events for deterministic tests. Platform injection is a separate
  integration test, not the golden path.
- Every platform quirk is documented per backend; no silent drops.

This extends [`input.md`](input.md) and [`os-events-v1.md`](../specs/os-events-v1.md).

## 3. Android integration

### 3.1 Surface and lifecycle

- Host the renderer in a `SurfaceView`/`TextureView` (or `NativeActivity`) and obtain an
  `ANativeWindow`.
- Create the Vulkan surface with `VK_KHR_android_surface` (`vkCreateAndroidSurfaceKHR`).
- Handle `SurfaceHolder.Callback`/surface lifecycle:
  - `surfaceCreated` -> create surface/swapchain
  - `surfaceChanged` -> resize/format
  - `surfaceDestroyed` -> stop rendering, cancel input, release surface
- Never render after `surfaceDestroyed`; recreate resources on the next surface.
- Use `AndroidSurface`/`ANativeWindow` acquire/release correctly; do not keep a stale window.
- Use `VK_PRESENT_MODE_FIFO_KHR` as the safe baseline; `Surface.setFrameRate` for content rate.
- Release GPU resources on background/trim; no background GPU work.

### 3.2 Input

Choose one input path and keep it consistent:

- **Custom `View`** overriding `onTouchEvent`, `onGenericMotionEvent`, `onHoverEvent`,
  `onKeyDown/Up`, `onCapturedPointerEvent`, `onFocusChanged`; or
- **`NativeActivity` + `AInputQueue`** (`AInputQueue_attachLooper`, `AInputEvent`) when using a
  native app shell.

Normalize:

- `MotionEvent` actions: `DOWN/MOVE/UP/CANCEL`, `POINTER_DOWN/UP`, `HOVER_MOVE/ENTER/EXIT`,
  `SCROLL`, `OUTSIDE`; pointer ids and indices; historical samples.
- Tool types: finger, stylus, eraser, mouse; axes: pressure, tilt, orientation, distance,
  scroll, relative X/Y.
- `KeyEvent`: action, keycode, scan code, meta state, repeat, canceled, unicode.
- `InputDevice` sources: touchscreen, mouse, stylus, keyboard, gamepad, rotary encoder.
- Focus: `onWindowFocusChanged`, `onFocusChanged`, `onTopResumedActivityChanged`.
- Pointer capture: `requestPointerCapture`/`releasePointerCapture`; release on cancel/background.
- System back: `OnBackInvokedCallback` (API 33+), `android:enableOnBackInvokedCallback`; do not
  break predictive back. Fallback for older APIs.
- Gesture conflicts: edge/back gestures, scroll containers, drag/drop; document precedence.

### 3.3 IME and text input

A custom-rendered text field does not get an IME for free.

- Provide an `InputConnection` from the focused custom view (`onCreateInputConnection`,
  `EditorInfo`), or bridge to an off-screen/platform `EditText` only as a documented fallback.
- Handle `setComposingText`, `commitText`, `setSelection`, `deleteSurroundingText`,
  `sendKeyEvent`, `performEditorAction`, `requestCursorUpdates`.
- Use `WindowInsets.Type.ime()` and `WindowInsetsAnimation.Callback` for layout/animation.
- Declare `android:windowSoftInputMode="adjustResize"` (or handle insets explicitly).
- Expose selection/caret/text to accessibility; support copy/paste/autofill/password managers
  where applicable.

### 3.4 Accessibility

Custom-drawn UI is invisible to TalkBack unless bridged.

- Implement `AccessibilityNodeProvider` or `ExploreByTouchHelper` (AndroidX) for the semantics
  tree.
- Expose role/name/description/value/state/actions, bounds, focus order, collection
  set-size/position, and live-region announcements.
- Handle `AccessibilityEvent` (focus, click, text changed, scroll, window state) and
  `AccessibilityManager` (touch exploration, enabled state).
- Support `FLAG_SECURE` for sensitive surfaces; screen capture/recording behavior is explicit.
- Test with TalkBack and Accessibility Scanner; custom surface coordinates must map to screen
  bounds correctly.

### 3.5 Configuration, multi-window, fold

- Android may **recreate the Activity** on configuration changes unless declared otherwise.
  Decide per app:
  - let it restart -> implement `onSaveInstanceState`/state restoration;
  - declare `android:configChanges` -> handle every listed change yourself (size, orientation,
    density, fontScale, locale, layoutDirection, uiMode, colorMode, screenLayout,
    smallestScreenSize).
- `onConfigurationChanged` must update `WindowMetrics`, style environment, layout, text,
  insets, and fold state.
- `onMultiWindowModeChanged`, `onPictureInPictureModeChanged`: adapt layout/pacing.
- `onTrimMemory`/`onLowMemory`: release caches/GPU memory; no crash.
- Fold/unfold may stop/restart the app; preserve selection/scroll/focus for continuity.
- Use `WindowInfoTracker`/`FoldingFeature` for posture/hinge/occlusion; see
  [`os-events-v1.md`](../specs/os-events-v1.md) and [`responsive.md`](responsive.md).

### 3.6 Display, refresh, safe areas

- `WindowManager.getCurrentWindowMetrics`, `WindowMetrics.getBounds`, `getWindowInsets`.
- `WindowInsets.Type.statusBars/navigationBars/systemBars/displayCutout/ime/systemGestures`.
- `DisplayManager` for display add/remove/change; `Display.Mode`/`Surface.setFrameRate` for
  refresh; `WindowManager.LayoutParams.preferredDisplayModeId` where appropriate.
- `WindowMetricsCalculator` (AndroidX) for current/maximum metrics.
- Handle cutout/gesture-bar/IME insets; never hardcode screen size.
- Fold posture is a metric, not a separate UI mode.

### 3.7 Android declarations

`AndroidManifest.xml` (representative; not exhaustive):

```xml
<uses-feature android:name="android.hardware.vulkan.version" android:required="true" />
<uses-feature android:name="android.hardware.vulkan.level" android:version="1" android:required="true" />
<uses-feature android:name="android.hardware.touchscreen" android:required="false" />
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />

<application
    android:allowBackup="false"
    android:supportsRtl="true"
    android:localeConfig="@xml/locales_config"
    android:networkSecurityConfig="@xml/network_security_config"
    android:enableOnBackInvokedCallback="true">

  <activity
      android:name=".MainActivity"
      android:exported="true"
      android:resizeableActivity="true"
      android:supportsPictureInPicture="true"
      android:screenOrientation="unspecified"
      android:windowSoftInputMode="adjustResize"
      android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout|density|fontScale|locale|layoutDirection|uiMode|colorMode"
      android:theme="@style/AppTheme" />
</application>
```

Rules:

- `configChanges` is a contract: if you list a change, you must handle it correctly; otherwise
  let the Activity restart and restore state.
- Declare `resizeableActivity`/PiP if you support multi-window; test freeform/desktop windowing.
- Declare Vulkan feature level/version if required; fall back gracefully if unavailable.
- Request runtime permissions at the point of use; explain denied/permanently denied.
- `networkSecurityConfig`/cleartext policy, backup/data-extraction rules, and foreground-service
  types are explicit.
- Android 15+ native libraries should be 16 KB page-size compatible; validate packaging.
- Provide `android:windowLayoutInDisplayCutoutMode`/edge-to-edge policy and handle insets.
- Add a privacy/data-safety declaration for collected data; no hidden telemetry.

## 4. iOS/iPadOS integration (deferred)

If iOS is added, the same contract applies, with different APIs.

### 4.1 Surface and lifecycle

- Host rendering in a `UIView` subclass whose `layerClass` is `CAMetalLayer` (or `MTKView`).
- Set `drawableSize` on resize/scale; `contentsScale = UIScreen.scale`.
- Use `CADisplayLink`/`CAFrameRateRange` or `MTKView.preferredFramesPerSecond`; respect ProMotion
  and low-power mode.
- Metal native or MoltenVK (ADR-010); no Vulkan-on-iOS assumption.
- `UISceneDelegate`/`UIViewController` lifecycle: pause/resume, background, memory warnings,
  trait/geometry changes.
- Release drawables/resources on background; no rendering when inactive/background.

### 4.2 Input

- `UIView`/`UIResponder`: `touchesBegan/Moved/Ended/Cancelled`; coalesced and predicted touches;
  force, altitude, azimuth, roll; pencil interactions.
- Hover/pointer: `UIHoverGestureRecognizer`, `UIPointerInteraction`; indirect input
  (`UIApplicationSupportsIndirectInputEvents`).
- Keyboard: `pressesBegan/Ended`, `UIKeyCommand`, `GCKeyboard`; gamepad `GCController`.
- IME/text: implement `UITextInput` (and `UIKeyInput`) on the custom view; handle marked text,
  selection, autocorrection, dictation, edit menu, undo/redo.
- System gestures/back: don't swallow edge gestures; provide navigation semantics.
- Gesture recognizer precedence vs custom handlers must be explicit.

### 4.3 Accessibility

- `isAccessibilityElement`, `accessibilityLabel/Value/Hint/Traits`, `accessibilityFrameInContainerSpace`.
- `UIAccessibilityElement`/container, `accessibilityElements`, custom actions/rotors,
  `accessibilityActivate`, `accessibilityIncrement/Decrement`, `accessibilityScroll`.
- `UIAccessibility.post(notification:argument:)` for announcements/focus.
- Test with VoiceOver, Switch Control, Accessibility Inspector, large text, reduced motion,
  high contrast.

### 4.4 Declarations

`Info.plist` / entitlements / privacy manifest (representative):

- `UIApplicationSceneManifest`, `UISceneConfigurations`, `UIApplicationSupportsMultipleScenes`
- `UISupportedInterfaceOrientations`, `UIRequiresFullScreen`
- `UIApplicationSupportsIndirectInputEvents`
- `UIAppFonts`, `UIRequiredDeviceCapabilities` (`metal`, `arm64`)
- `NS*UsageDescription` for camera/mic/photos/local network/tracking
- `NSAppTransportSecurity` policy; no insecure exceptions without justification
- `UIBackgroundModes` only if genuinely needed
- `PrivacyInfo.xcprivacy` (required-reason APIs, collected data, tracking)
- Entitlements: keychain, push, associated domains, network, iCloud as needed
- App Store privacy nutrition labels; ATT permission if tracking.

## 5. What mobile requires from rendered components

The OS sees one surface; Tessera owns the component contract. A component is only "native
enough" if it provides:

| Concern | Requirement |
|---|---|
| Accessibility | semantics role/name/state/actions, bounds, focus order, announcements |
| Text input | IME composition/commit, selection/caret, autocorrect/dictation, edit menu |
| Focus/keyboard | focus ring, tab/arrow navigation, key commands, gamepad/D-pad |
| Pointers/touch | multi-touch, pointer ids, hover, stylus pressure/tilt, capture/cancel |
| Scrolling | platform-like physics/overscroll, nested scroll, scroll anchoring; virtualize large lists |
| System gestures | back/home/recents/edge gestures not swallowed; predictive back |
| Insets/safe areas | status/nav bars, cutout, IME, gesture bars, fold hinge |
| Adaptive layout | orientation, multi-window, PiP, fold posture, external display |
| Theme/accessibility | dark mode, high contrast, reduced motion, text scale, RTL |
| Editing | copy/paste, selection, drag/drop, context menu, autofill/password managers |
| Privacy/security | `FLAG_SECURE`, screen capture, clipboard policy, TLS, permissions |
| Lifecycle | pause/resume/background, surface loss, state restore, memory pressure |
| Power | pause rendering when hidden, cap FPS, thermal/battery saver, no background GPU |

If a custom component cannot meet these, use a platform-native control or a documented bridge
instead of pretending the custom surface is enough.

## 6. Event-fidelity test matrix

- Normalization: touch/mouse/stylus/keyboard/gamepad; historical/coalesced samples; pressure/tilt.
- Cancellation: `ACTION_CANCEL`/`touchesCancelled`; pointer capture loss; focus loss.
- Lifecycle: background/foreground; surface destroy/recreate; activity/scene restart.
- Configuration: rotation, resize, density/fontScale, locale/RTL, dark mode/color mode.
- Multi-window/PiP/fold: resize, posture, hinge occlusion, app continuity.
- IME: composition/commit/cancel, selection, keyboard show/hide insets, dictation.
- Accessibility: TalkBack/VoiceOver focus, actions, announcements, collection position.
- System gestures: back/predictive back, edge gestures, home/recents; no swallowed events.
- Power/thermal/memory: trim/low-memory, battery saver, thermal, background limits.
- Real devices: Pixel/Samsung/foldables; iPhone/iPad; OEM quirks; not just emulators.

## 7. Status and open questions

Status:

- Android/iOS backends are **not implemented**.
- This doc defines the contract; implementation is a future Big Step (mobile).
- Existing docs cover normalized events, HAL, resize/fold, input, accessibility, responsive,
  pacing/power, packaging, and virtualization.

Open questions:

- Android input path: custom `View` vs `NativeActivity`/`AInputQueue`; one or both?
- IME bridge: custom `InputConnection` vs off-screen `EditText` fallback.
- Accessibility bridge scope for custom-drawn charts/text.
- Android `configChanges` policy per app vs always restart/restore.
- iOS target: Metal native vs MoltenVK; Vulkan feature parity.
- iOS `UITextInput` scope and edit-menu/autofill support.
- Background/foreground policy per platform (no background rendering).
- Packaging: ABI splits, 16 KB pages, native libs, privacy manifests.
- Test automation for custom surfaces (instrumentation/UIAutomator/XCUITest).
- Store policy/accessibility review requirements.

## 8. Related documents

- [`os-events-v1.md`](../specs/os-events-v1.md) — normalized OS events/display/fold
- [`hal.md`](hal.md) — window/display/input/power/lifecycle interfaces
- [`window-resize.md`](window-resize.md) — resize/DPI/lifecycle separation
- [`input.md`](input.md) — normalized input/focus/capture/IME
- [`accessibility.md`](accessibility.md) — semantics tree/platform bridges
- [`responsive.md`](responsive.md) — safe areas/fold/multi-window
- [`game-loop.md`](game-loop.md), [`performance.md`](performance.md) — pacing/power
- [`virtualization-v1.md`](../specs/virtualization-v1.md) — large lists/grids
- [`security.md`](security.md), [`packaging.md`](../guides/packaging.md) — privacy/declarations
