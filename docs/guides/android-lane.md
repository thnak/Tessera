# Android lane

How to cross-compile Tessera for Android and run its tests on an emulator. The lane is advisory
(nightly and on demand, [`.github/workflows/android.yml`](../../.github/workflows/android.yml)) and
Android is not a V1 platform ([ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md),
Big Step F). What exists today is the **null backend** and the test executables; there is no
`AndroidManifest.xml`, no Gradle project, no `ANativeWindow`/`NativeActivity` glue and no
`VK_KHR_android_surface` path, so no runnable Android app can be produced yet.

## 1. Use the NDK the workflow uses

The workflow resolves `ANDROID_NDK_LATEST_HOME`, which is currently **NDK 30**. Older NDKs fail to
compile the tree: NDK 28's libc++ ships `to_chars` for floating point but **no** `from_chars`
overload, so every `std::from_chars(..., double, std::chars_format)` call site (for example
`src/harness/json_parse.cpp`) is a hard error. That is a toolchain gap, not a code defect — do not
rewrite the call sites to work around it.

```powershell
$ndk = "$env:LOCALAPPDATA\Android\Sdk\ndk\30.0.16248370"
cmake -S . -B build/android-x86_64 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" `
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static `
  -DCMAKE_BUILD_TYPE=Release -DTESSERA_BUILD_TESTS=ON -DTESSERA_BUILD_VULKAN=OFF `
  -DTESSERA_BUILD_POC=OFF -DTESSERA_WARNINGS_AS_ERRORS=ON
cmake --build build/android-x86_64 --parallel
```

`ANDROID_STL=c++_static` matters: the test binaries are pushed to the device and run there, so they
must not depend on a shared libc++ the image may not carry.

The arm64-v8a configuration is the one the workflow builds for shipping; it uses
`-DANDROID_ABI=arm64-v8a -DTESSERA_BUILD_TESTS=OFF` and cannot run on an x86_64 emulator.

## 2. Run the tests on an emulator

Any x86_64 AVD works; the workflow pins API 30, and a newer image is fine for local runs.

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$emulator = "$env:LOCALAPPDATA\Android\Sdk\emulator\emulator.exe"
& $emulator -avd <name> -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect
& $adb wait-for-device
& $adb shell getprop sys.boot_completed   # 1 means booted
```

Push `tests/unit/*_tests` and `tests/gpu/*_tests` to `/data/local/tmp/tessera`, `chmod +x` them, and
run them through [`scripts/ci/android_emulator_tests.sh`](../../scripts/ci/android_emulator_tests.sh),
which owns the skip list.

**A test that reads a repository path belongs on that list.** The build bakes absolute
`CMAKE_SOURCE_DIR` paths into the binary, so on the device it fails with a "not a directory" message
rather than a real assertion. `corpus_measure_tests` is the current example; the other host-path
readers (`ir_json_tests`, `lowering_golden_tests`, `schema_golden_tests`) were already listed.

## 3. Vulkan cross-compiles, but there is no window yet

`-DTESSERA_BUILD_VULKAN=ON` now cross-compiles for Android. Two things had to change for that:

- **The shader manifest checker is built for the host.** The shader rules run
  `tessera_shader_manifest_check` at build time, and a cross-compiling build used to produce a
  *target* binary the host cannot execute, so every rule failed with
  `'tessera_shader_manifest_check' is not recognized`. The top-level `CMakeLists.txt` now builds it
  in a separate project (`tools/shaders/host`) through `ExternalProject_Add` and points
  `TESSERA_SHADER_MANIFEST_CHECK` at the host binary. The host generator is chosen from
  `CMAKE_HOST_WIN32`, not `WIN32`: in an Android configure `WIN32` is false because it describes the
  target.
- **`SurfacePlatform::Android` exists** (RHI 1.10). `vulkan_present_platform.cpp` resolves it, and
  `vulkan_swapchain_surface.cpp` creates the surface with `vkCreateAndroidSurfaceKHR`. There is no
  `vkGetPhysicalDeviceAndroidPresentationSupportKHR` for this family: the platform guarantees that a
  graphics-capable queue family can present to an `ANativeWindow`, so the probe is the queue family
  itself.

### 3.1 The window backend adopts a surface, it does not create one

Android has no window manager a native process can create a window in: the OS owns the surface and
hands the app an `ANativeWindow*` (from a `SurfaceView`/`TextureView` holder, or `NativeActivity`).
So `src/platform/android/window_android.cpp` **adopts** the handle the host already owns:

```cpp
std::unique_ptr<hal::Window> create_android_window(ANativeWindow* window,
                                                   const WindowDesc& desc, float dpi_scale);
```

`hal::create_window()` keeps returning `nullptr` on Android, which is the documented "no backend"
answer: no `WindowDesc` can produce a surface on its own. A host calls `create_android_window`
instead, and the backend takes its own `ANativeWindow_acquire` reference so the host may release
its own at any point.

The backend reports the surface extent, the density scale and `WindowSystem::Android`, and revokes
the borrowed `NativeSurface` when the host replaces or closes the surface — a renderer can never
present into a dead window. It does **not** translate input (`MotionEvent`/`KeyEvent` arrive through
the host's own callbacks; `set_input_queue` stores the queue for that slice) and does **not** report
insets, cutouts or fold state (those need `WindowInsets`/`WindowMetricsCalculator` over JNI). The
metrics carry the documented zero-inset fallback with the "assumed" bits set, so a consumer cannot
mistake an unwired source for a measurement.

### 3.2 The window driver builds for Android

`tessera_app_window` used to be gated on `NOT CMAKE_CROSSCOMPILING`, so it did not exist in an
Android build. The gate now admits Android (`NOT CMAKE_CROSSCOMPILING OR ANDROID`), and the driver
reaches the surface through an opaque token rather than an Android type:

```cpp
// include/tessera/hal/native_surface_token.hpp
struct NativeSurfaceToken { void* token = nullptr; };

// include/tessera/app/window.hpp
struct WindowOptions {
    hal::NativeSurfaceToken surface;  // null on every platform that creates its own window
    float dpi_scale = 1.0f;
};
```

The token is opaque for the same reason `AccessibilityHostHandle` is: only a platform translation
unit may interpret it, so `include/tessera/app/window.hpp` stays OS-free (ADR-020 rule 1). On
Android it is the `ANativeWindow*` from the `SurfaceView`/`TextureView` holder or `NativeActivity`,
and `hal::create_window` returns `nullptr` when it is null — the documented "no surface yet" answer.

### 3.3 The app shell

`android/` holds the Gradle project and the native entry point:

| Path | What it is |
|---|---|
| `android/app/src/main/AndroidManifest.xml` | the activity, the Vulkan feature requirements, and the `configChanges` contract |
| `android/app/src/main/cpp/scene.hpp` | the shell's scene and stylesheet: a card, a tap counter and a text field |
| `android/app/src/main/cpp/CMakeLists.txt` | the `tessera_android_app` shared library, linking `tessera_app_window` |
| `android/app/build.gradle.kts` | compiles the engine through the same CMake project and packages the APK |

The scene outlives the host: the shell creates it once and creates/destroys the host around it as
surfaces come and go, so a backgrounded app keeps its state (the tap counter survives a
background/foreground cycle). The host borrows the root and owns the compiled stylesheet, which is why
the scene is a `unique_ptr` the shell holds rather than a local.

The manifest declares only what the engine can honour. `localeConfig`, `networkSecurityConfig`,
`enableOnBackInvokedCallback` and `POST_NOTIFICATIONS` are deliberately absent: declaring a capability
the app cannot honour routes platform events to a handler that does not exist.

### 3.4 Input translation

`src/platform/android/android_input.cpp` translates `AInputEvent`s into `ui::InputEvent` and pushes
them into the queue `set_input_queue` installed. The shell forwards from `onInputEvent` through
`hal::Window::translate_input`, which takes an opaque `const void*` so no platform type reaches a
public header.

The mapping rules live in `android_input_translation.cpp`, which **includes no NDK header**: the
action, tool-type, source and keycode constants are stable ABI values, so the rules are unit-tested
on every lane (`android_input`). That is where the defects that a device lane cannot see cheaply are
caught — the scroll sign, the pointer-index packing, the cancel semantics.

- **Scroll sign.** Android's `AXIS_VSCROLL` is positive when the content should move *up*; the engine
  uses the web sign, where positive `dy` scrolls content *down*. The vertical axis is negated.
- **Pointer identity.** Android reports a pointer *index* per event, and the index is renumbered as
  fingers lift. The translator fixes the id at `ACTION_DOWN` and reuses it for the gesture.
- **Cancel.** `ACTION_CANCEL` and `ACTION_OUTSIDE` both end the gesture, so the engine releases its
  capture instead of keeping a stale active state.

The NDK exposes **no `AKeyEvent_getUnicodeChar`** (only the Java `KeyEvent` has one), so typed text
is derived from the keycode: letters (with shift for case), unshifted digits and space. Punctuation,
non-ASCII and composed text come from the IME bridge, not from a keycode guess — guessing would be
wrong for every non-US layout.

Not carried yet: historical/coalesced samples (`AMotionEvent_getHistorical*`), pressure tilt and
orientation. The newest sample is used, which loses sub-frame motion but never loses an event.

### 3.5 Window metrics: insets, cutouts and fold

`src/platform/android/android_metrics_translation.cpp` decodes the platform's metrics into
`hal::WindowMetrics`. Like the input rules it **includes no Android header**, so it is
unit-tested on every lane (`android_metrics`).

The platform reports insets in **physical pixels**; `WindowMetrics` carries logical pixels.
That conversion is why this is a separate testable unit: a wrong scale turns a 24 px status
bar into a 24 dp one, a layout bug that only shows at a non-1.0 density. The decode
**rounds** rather than truncates, so a 2.75x device does not lose a pixel per side.

- **Cutout rectangles.** Android reports `right`/`bottom` as exclusive edges; the engine's rectangles
  are `x`/`y`/`width`/`height`, so the extent is the difference. A zero-area rectangle is dropped
  rather than reported as a cutout that insets nothing.
- **A flat fold does not occlude.** A flat fold is a seam, not a barrier: it reports
  `FoldState::Flat` and its axis, but `FoldOcclusion::None`. Only `HalfOpened` reports an
  occluding hinge, and only when the platform gives it a non-zero rectangle.
- **Orientation comes from the extent**, not Android's rotation, so a device that reports
  neither still gets the same answer as the desktop lanes.
The backend exposes `set_insets`, `set_cutouts` and `set_fold` for the host to call from its
platform callbacks; each queues at most one coalesced `SafeAreaChanged`/`FoldChanged` event.

### 3.6 The JNI bridge

The shell is a **Java activity**, not `android.app.NativeActivity`, because insets, cutouts
and fold posture are Java APIs with no NDK equivalent: a NativeActivity can hand over an
`ANativeWindow` but cannot read those.

Three files carry it: `TesseraActivity.java` (edge-to-edge, hides the system bars, owns the view),
`TesseraView.java` (a `SurfaceView`: surface lifecycle, insets/cutouts, touch) and
`jni_bridge.cpp` (the only TU that includes `<jni.h>`).

`TesseraView` is a `SurfaceView` because that is the one view whose `Surface` the NDK can turn into
an `ANativeWindow` (`ANativeWindow_fromSurface`). A `TextureView` would give a `SurfaceTexture`,
which needs a different Vulkan path.

**The render thread.** `WindowHost` is single-threaded and must be created, run and destroyed
on the thread that owns the window, so the bridge starts a dedicated thread that calls
`host->run(0)` (3.19 has the pause rules). `pump_once` on the Java thread would need a
battery-burning spin.

**The input seam.** The shell is a Java activity, so touch arrives as an `android.view.MotionEvent`,
not as the NDK's `AInputEvent`. The bridge reads the getters once into a `MotionSample` and the pure
rules in `android_motion_translation.cpp` do the mapping — the same split as the native path, and for
the same reason: the rules are plain numbers, so the physical-to-logical conversion, the scroll sign
and the pointer bookkeeping are unit-tested on every lane (`android_motion`) instead of only on a
device.

Two seams carry it: `post_input_event` queues an event the HOST translated, and
`translate_input` is the opposite split where the backend translates an opaque native event.
Android has both because the same platform can deliver touch either way.

**The IME bridge.** A custom-rendered field gets no IME for free: the platform talks to an
`InputConnection` and the engine consumes `ui::InputEvent`. `TesseraInputConnection` is the
adapter and holds no text of its own - the engine owns the buffer, undo history and selection,
and a Java copy would be a second source of truth that drifts.

Two other hooks are required, and both are easy to miss:

- `onCheckIsTextEditor()` must return true, or the platform never offers the view a keyboard.
- The show request is explicit (`showSoftInput`), because a `SurfaceView` cannot focus itself
  like an `EditText`. It is also **conditional**: the view asks the engine whether the tap landed
  on the field (`nativeTapOnField`), because the whole window is one surface and the platform
  cannot know which part is editable. Without it the keyboard appeared on every tap; a tap
  elsewhere now dismisses it.

The mapping rules live in `android_ime_translation.cpp` and are **pure over UTF-16** - no SDK,
no JNI - so `android_ime` pins the two conversions that fail silently on screen: **UTF-16 to
UTF-8** (a naive per-character loop encodes each surrogate half separately and turns every emoji
into two replacement characters) and **UTF-16 offsets to UTF-8 byte offsets** (Android reports
the caret in UTF-16 units, the engine's selection is UTF-8 bytes, so mixing them puts the caret
in the wrong place on any non-ASCII string).
A lone surrogate becomes U+FFFD rather than being dropped, because dropping it would desynchronise
every offset after it.

**The engine owns the buffer, so the platform must ask.** Forwarding callbacks is half an IME
bridge: the platform also *queries* the text, and a bridge that answers `""` makes the IME
behave as if the field were always empty - suggestions stop, Backspace deletes nothing, and the
composing region sits at offset 0. `TesseraInputConnection` overrides the queries too:
`getTextBeforeCursor`/`getTextAfterCursor` read the buffer around the caret in UTF-16 units,
`getSelectedText` reads `EditBuffer::selected_text()`, `setSelection` converts UTF-16 offsets to
UTF-8 bytes, and `deleteSurroundingTextInCodePoints` takes the same delete path as the UTF-16
variant.
The shell owns one `EditController` and one `TextInputRuntime` (`Shell::editor`,
`Shell::editor_runtime`). The runtime is attached to the field's node, so a committed edit
publishes the buffer's text **and** the caret into that node's `PaintData` - otherwise a field
whose node started empty paints nothing while the buffer fills.
and `ui::EventType` is the scene's; they do not share values (`EditText` is 13 in one and 14 in
the other). A `static_cast` turns a commit into a composition start, which looks like typing
doing nothing while the buffer stays empty. The bridge maps the six types and drops the rest.

**The metrics seam.** `hal::Window` gains `set_physical_insets` and `set_physical_cutouts`,
taking the platform's **physical** pixels. The backend converts them with the decode rules in
`android_metrics_translation.cpp`, so the bridge never knows the layout rules and the conversion
stays unit-tested on every lane.

### 3.7 What is still missing

- **Accessibility's long tail.** The role/action mapping, the pull-side provider, the Java
  `AccessibilityNodeProvider`, `SetText` and collection set-size/position (3.9) are done. Not wired:
  live regions, the coalesced announcements, and `SetSelection` - the engine's `Action` has no caret
  member, so that request is delegated rather than accepted and ignored.
- **The IME's long tail.** `requestCursorUpdates` answers once (3.18); only the async cursor stream,
  which the engine has no notion of, remains.
- **A generated component.** The shell's scene is hand-built: the codegen CLI runs on the host and
  `tessera_compiler_demo` is gated on `NOT CMAKE_CROSSCOMPILING`; a generated `demo::Demo` needs the
  codegen host tool, the pattern the shader manifest checker already uses.

### 3.8 Emulator GPU mode matters

A lane that exercises the real renderer must boot with `-gpu host`. The CI default
(`-gpu swiftshader_indirect`) reports Vulkan 1.2.0, below the 1.3 minimum `choose_device`
enforces, so the app refuses to start.

### 3.9 Accessibility: the one platform that pulls instead of pushing

Windows (UIA) and Linux (AT-SPI) are **push** bridges: a bridge thread on the app side calls out
and the app answers from a tree it already published. Android is the opposite.
`AccessibilityNodeProvider` is **pull**: the platform calls
`createAccessibilityNodeInfo(virtualViewId)` on the UI thread and expects that node back before
the call returns. A UIA-shaped bridge would block the platform on a frame, which the
no-blocking rule forbids. So the Android column is a different shape, not a branch:
So the Android column is a different shape, not a branch in an existing one:

- **`ui::a11y::SnapshotProvider`** keeps the last published snapshot and answers one node at a time.
  Its mutex covers one bounded copy (at most `kMaxNodes` entries), never a snapshot build, so the
  render thread and the platform thread never wait on each other for longer than that copy.
- **`AccessibilityBackend::Provider`** is the driver's third backend. It reuses the controller's
  existing dirty tracking and action drain, so a static window still builds no tree and publishes
  nothing. The shell sets `WindowOptions::accessibility_provider`; a null provider reports
  `UnsupportedPlatform`, which is the documented "accessibility off, window keeps running" path.
- **`TesseraAccessibilityProvider`** (Java) is what the platform talks to. It holds **no tree**:
  each query is one JNI call returning the node's class name, text, description, flags, bounds,
  action ids and child ids, copied into an `AccessibilityNodeInfo`.

Three details that are easy to get wrong, and that the tests pin:

**Virtual ids must be stable.** A snapshot index is a pre-order position, so it renumbers on
every rebuild and a screen reader focused on view 3 would point at a different control. The
provider keeps a slot table: a node keeps its id while it stays in the tree; id `0` is the host.

**The action ids are the platform's.** They are *not* consecutive powers of two:
`ACTION_FOCUS` is 1, `ACTION_CLICK` is 16, `ACTION_SCROLL_FORWARD` is 4096,
`ACTION_SET_TEXT` is 2097152. Numbering them in enum order turns a click into a focus
request, and since the platform echoes back the id it was offered, that failure is silent.
They were read from `android.jar` with `javap -constants` and the test asserts them. They are
also a **list**, not a bit set: `Click | LongClick` is 48, no id the platform knows.

**Bounds are the engine's logical pixels**, the view's own coordinate space, so Java converts
no density; they cross as fixed point in 1/64 px so a fractional position rounds once.
```powershell
# What a screen reader should see, without a screen reader:
adb shell settings put secure enabled_accessibility_services \
  com.android.talkback/com.google.android.marvin.talkback.TalkBackService
adb shell dumpsys accessibility | Select-String "tessera"
```

The emulator check that matters: with TalkBack on, swiping right must move focus through *"Tessera on
Android"*, *"Taps: 0"*, *"Tap me"* and the text field — four separate stops. Before this slice the
whole window was one unnamed stop.

### 3.10 Fold posture: the one AndroidX dependency, and why

**The platform has no fold API.** Verified against the build's own `android.jar` for API 35,
36.1 and 37: no class matches fold, hinge or posture, and `android.view.WindowInfo` does not
exist at any of those levels. `android.view.DisplayShape` does exist but describes the outline
of a display, not a posture. So `androidx.window:window` is the only source, and it is the
lane's single AndroidX dependency.
Posture is exposed only by **Jetpack WindowManager** (`androidx.window:window`), Google's official
Apache-2.0 library for exactly this. It was already the planned dependency:
[`dependencies.md`](../specs/dependencies.md) lists it as "Android V1 optional" and
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) already carried the entry, so this slice
consumes a decision that was already made rather than making a new one.

The shell pins **1.5.1** and uses it for one call. The chain it pulls in is `window-core`,
`androidx.core`, `androidx.collection`, `androidx.annotation`, `jspecify`, `kotlin-stdlib` and
`kotlinx-coroutines-android` - which is why the build also enables the Kotlin plugin.

**Why a Kotlin file.** `WindowInfoTracker.windowLayoutInfo` returns a Kotlin `Flow`; collecting
one from Java means suspend functions through `BuildersKt` and a hand-written `Function2`, which
breaks on any coroutines bump. `FoldObserver.kt` does it in three lines and the mapping stays in
`TesseraFoldReader.java` with the rest of the shell.

**Two traps.** A fold change is *not* always a configuration change - the emulator's
`HALF_OPENED` changes the hinge angle without resizing the window - so a re-read on
`onConfigurationChanged` alone reports the launch posture forever. And `onResume` runs *before*
`surfaceCreated`, so a fold read there has no handle; `whenNativeReady` defers the first push.

Verification on the emulator: `cmd device_state state 1` is `HALF_OPENED` and `state 2` is
`OPENED`. Both report posture 1/2 with axis 2 (Vertical) and hinge `1038,0,1038,2152` - the centre
of the 2076 px display - observed live without restarting the activity.

### 3.11 Hinge angle: the one fold signal the platform SDK does have

Posture is discrete and needs AndroidX. The **angle** is continuous and does not: the platform
exposes it as a sensor, `android.hardware.Sensor.TYPE_HINGE_ANGLE` (36), present in
`android.jar` for API 35 and later and in the NDK as `ASENSOR_TYPE_HINGE_ANGLE`. So this slice
adds no dependency - it is `SensorManager`, which every Android build already has.

**Why a separate reader.** The two read different sources at different rates, and a device can
have one without the other. Posture comes from `FoldingFeature` as `FLAT`/`HALF_OPENED`, one
event per transition; the angle comes from `SensorManager` as 0-360 degrees, continuous while
folding. A foldable with no hinge sensor still reports posture, and a device whose window is
not laid out yet reports no posture while the sensor keeps working.
`HingeAngleChanged` is a separate event for the same reason: a posture change is a discrete relayout,
the angle is a continuous signal a scene may animate against, and merging them would rebuild the
layout on every degree. The angle coalesces into the newest, so a burst between two ticks costs one
event.

**The sentinel is negative.** `kUnknownHingeAngle` is `-1`, because `0` is a real measurement
- a fully closed hinge. A non-finite reading becomes the sentinel rather than a clamp, so a
broken HAL reporting `NaN` cannot look closed, and the upper clamp is 360 because a display
that folds all the way around is real. `fold_info` sets the angle before the posture switch,
so a device with a sensor but no `FoldingFeature` still reports it.
**Registration follows the lifecycle.** The sensor is registered on resume and released on
pause: a non-wake-up sensor left registered while backgrounded stays powered for a value no
frame can show.
Verification on the emulator, which has a hinge sensor:

```powershell
adb shell dumpsys sensorservice | Select-String "hinge"
# 0x0000000b) Goldfish hinge sensor0 (in degrees)
#   type: android.sensor.hinge_angle(36)
```

It reports `90.00` at `HALF_OPENED` and `0.00` at `CLOSED` - the sensor's scale, not the enum.

### 3.12 Keys and the caret: two seams a custom-rendered field does not get for free

A custom-rendered text field gets neither of these from the platform, and both fail in a way that
looks like "the engine is broken" rather than "a seam is missing".

**Keys.** A `SurfaceView` receives key events only when it is focused, and the engine draws its own
field, so the platform cannot route a key to "the field" — it routes it to the view. Without a
`dispatchKeyEvent` override the engine never sees a Backspace at all: the IME's own suggestion strip
drops a character while the field keeps the text, which is exactly the symptom that was reported.
`TesseraView.dispatchKeyEvent` forwards every key except the system Back to `nativeKey`, which reads
the Java getters into a `KeySample` and lets the pure `translate_key_sample`
(`android_input_translation.cpp`) do the mapping — the same split as the touch path, so
`android_input` pins it on every lane.

Two rules matter there. A key-down that types a character produces **two** events, the key
event and a `Text` event; the edit controller inserts from the latter, so a bridge forwarding
only the key event types nothing. And a key the engine has no logical code for produces
**nothing**, so the caller lets the platform handle it - that is what keeps system Back working.

An IME that synthesises a key (Space, Enter) does **not** go through the view's dispatcher: the
IME owns the input connection, so the event arrives at `sendKeyEvent` instead. That override
takes the same `nativeKey` path, which is why Space types at all - without it the field could
not contain a space while letters (committed as text) still worked.

Enter has two paths. A hardware Enter arrives as a key event; a soft-keyboard Enter on a
multi-line field arrives as `performEditorAction`, which the connection answers by sending
the same key pair. A single-line field gates the newline off by design (submit is a form
concern the engine defers), so the shell's demo field is multi-line - otherwise Enter reads
as a broken key on a phone with no form to submit.

`adb shell input keyevent` cannot reproduce a newline here: it delivers only the key-up to
the view (the default `View.onKeyDown` consumes the down). Tap the soft keyboard's Enter.

**The caret.** `CaretBlink` is host-owned (`WindowHost::Impl::caret`) and ticks once per
driver iteration, but only the app knows whether a field is focused. Two options close the
loop:
`caret_active` (app to host: the focus/editability policy, asked every iteration) and
`caret_target` (host to app: the `TextInputRuntime` whose decoration carries the caret).
Without the first the blink never starts; without the second it starts but nothing it decides
reaches `TextDecoration::blink_phase`, so paint draws a solid caret forever. Both are null by
default, keeping every desktop caller on the pre-blink behaviour. `caret_blink_target` pins
the round trip.
### 3.13 A tap places the caret

A custom-rendered field does not get caret placement for free either. The platform reports a touch
as a point in view coordinates; the edit controller wants a `TextHit` (a UTF-8 byte offset plus a
caret affinity). `hit_test_point` already does the hard part - line selection by y, then x inside
that line's visual runs, bidi-aware and snapped to a grapheme boundary - but it takes a `TextStyle`,
a `TextConstraints` and a `FontMetrics`, and the bridge has none of those.

`android_caret_translation.cpp` is the adapter, and it is **pure**: no NDK, no JNI, no device. The
box arithmetic is where a caret lands one character off, so `android_caret` pins it on every lane:
physical pixels / density then minus the box origin (the layout is logical); the point measured from
the **content** box (a tap in the padding clamps, never resolves negative); a point outside the
border box is `inside == false` (a tap elsewhere leaves the caret alone); a zero density treated as 1
(dividing by it would make every point NaN).

Two things are easy to get wrong. **The density is already applied**: `translate_motion_sample`
divides the platform's physical pixels by the density, so passing it to `caret_from_point` again
divides twice - the first version of this slice did exactly that, and the caret did not move at all.
**The hit test must measure the glyphs paint draws**: the shell loads the same face paint uses
(`/system/fonts/Roboto-Regular.ttf`) and keeps a `FreeTypeFontMetricsProvider`, rather than guessing
metrics.

`caret_bridge.cpp` holds the shell half (the field's box, the blink policy, the placement call) so
`jni_bridge.cpp` stays inside its size budget.

### 3.14 Long-press and drag-to-select

The other half of the gesture is a long-press that starts a selection and a drag that extends it. The
platform does not report either one: it reports raw DOWN/MOVE/UP samples with timestamps, and the app
decides what they mean. That decision is `SelectionGesture` (`android_selection_gesture.cpp`), and it
is **pure** - no NDK, no JNI, no device - so `android_selection` pins the thresholds and the
transitions on every lane.

The thresholds are the platform's own, so a long-press feels like every other app: 500 ms
(`getLongPressTimeout()`) and 8 dp (`getScaledTouchSlop()`). Each rule is a defect if wrong: a move past
the slop before the timeout abandons the press (else every drag selects); a move inside the slop keeps
it alive (else a jittering finger never selects); a new down resets a gesture that never saw its up
(else a dropped up turns the next tap into a selection);
`tick()` also checks the timeout (a still finger sends no samples, so `feed()` alone never fires).
`SelectionGesture` is deliberately not `MotionGesture`: that one tracks pointer identity for the
input queue, and the selection thresholds must not affect its capture bookkeeping.

**Three seams it needs, all now in place.** The shell wires the state machine into `nativeOnTouch`
and ticks it from the caret callback:

1. **A wake seam.** `request_redraw` was a no-op on Android, so a static scene never woke to run the
   tick - and a finger that holds still sends no `MotionEvent`, so the timeout had nothing to fire
   on. `wait_for_events` is now interruptible: `request_redraw` sets a flag under its own mutex and
   notifies a condition variable, and the wait keeps the same bounded timeout, so a caller that
   never asks for a frame still throttles. Measured on the emulator: six screenshots 250 ms apart
   show the caret pixel alternating visible/hidden.
2. **The right box.** `caret_field_for_shell` read `node.bounds()`, which is **parent-relative**,
   while a touch is **window-relative**. The field sits 24 px inside its parent, so every caret
   landed 24 px right of the finger: a tap mid-word resolved past the end of the line and a drag
   selected nothing. `world_bounds()` is correct, and `android_caret` pins the contract.
3. **The right advances.** The hit test must measure with the **same advance source paint uses**
   (`FontPaintTextMetrics`, FreeType + HarfBuzz); stub advances do not match a real face, so a tap
   mid-word resolves to the end of the line even with the box correct.

### 3.16 Copy, Cut, Paste and Select all: a phone has no Ctrl key

`hal::Clipboard` is synchronous, and so is Android's `ClipboardManager` - but it needs a `Looper`
thread and the engine's render thread has none. The backend caches: a **write** updates the engine's
copy immediately (so a copy-then-paste in one frame sees it) and posts the platform write to the UI
thread; a **read** returns the cache, refreshed on `OnPrimaryClipChangedListener`.
Two entry points reach it: `KEYCODE_COPY`/`CUT`/`PASTE` (the platform's text toolbar and a
hardware keyboard) and `InputConnection.copy()`/`cut()`/`paste()` (the IME's context menu).
The key codes are **commands, not logical keys** - `translate_key_sample` drops them - so
`dispatchKeyEvent` handles them first; `Select all` reaches the buffer the same way.
The chord exposed a defect: `codepoint_from_key` derived a character from every letter key
regardless of modifiers, so `Ctrl+A` produced BOTH a `KeyDown` (Select all) and a `Text` event
(insert) - every chord also typed its letter. Control/Alt/Meta chords now type nothing.

`android_clipboard_text.cpp` does the UTF-8/UTF-16 conversion, and it is **pure** so
`android_clipboard_text` pins the rule that bites: a code point above U+FFFF is a **surrogate pair**,
two UTF-16 units. One unit per code point turns every emoji into a mojibake box and makes every offset
after it wrong. An invalid byte becomes U+FFFD rather than being dropped, so the lengths always agree.

### 3.17 The floating toolbar is the platform's, not the engine's

A selection normally raises a floating toolbar with Copy/Cut/Paste/Select all. The engine has
no popup surface and does not need one: Android's `ActionMode` is exactly that toolbar, it
positions itself, and it dismisses on a tap outside without the engine tracking anything.

The engine owns the selection, so the platform asks rather than tracking it.
`nativeSelectionState` fills a `float[3]` (has-selection, anchor x, anchor y in logical pixels)
- one call rather than three, so the selection cannot change between the "is there one" and
"where" reads. `syncSelectionToolbar` runs after every gesture that can change it (an up, and
a move, which is what a drag sends) and starts or finishes the `ActionMode` to match.
`onPrepareActionMode` hides Copy and Cut when there is no selection; every action finishes
the mode, because each collapses or changes the selection.

Verified on the emulator: long-press and drag over `hello` raises the toolbar above the field; Copy
writes `hello` (the platform's clipboard chip confirms it); Select all highlights the whole buffer.

### 3.18 The cursor anchor: where the IME puts its strip

The IME places its suggestion strip, autocorrect highlight and magnifier from the **cursor
anchor**. Without one it guesses, and on a custom-rendered field it guesses wrong: Gboard's
strip landed off the field. `requestCursorUpdates` answers from `nativeCaretGeometry`, which
returns the box in logical pixels - the **same** box paint draws the caret in, so anchor and
glyphs cannot drift.

Two rules. The platform wants **physical** pixels, so Java multiplies by the density once.
And the request is answered **immediately** rather than reported unhandled: the engine has no
asynchronous cursor stream, so a monitor request gets one truthful update and the IME polls
again later. Reporting "not handled" is what sent Gboard back to guessing.

### 3.19 Going to the background: pause the loop, do not tear it down

Home does **not** destroy the surface on current Android. Only `onPause`/`onStop` run, so
`surfaceDestroyed` never fires and the render thread kept presenting into a queue the
compositor stopped consuming. That wedged the queue, the task's snapshot never completed, and
coming back showed a starting window over an app with no input focus that ANR'd on every touch.

**The first fix was wrong.** It stopped the host on pause: close the window, join the render
thread. That deadlocked the UI thread, because a loop inside a present cannot return until the
queue is released - which needs the compositor the app no longer has. Detaching on timeout
instead leaked a live renderer holding its swapchain, so the next cycle had two renderers on
one queue and the app came back **black**.

The engine already models this: `Window::hide()` sets `WindowMetrics::minimized` and the loop
idles with zero presents, so nothing has to be joined. Three things had to follow:

1. **`hide()`/`show()` must queue a lifecycle event.** They only mutated the metrics, so the host -
   which acts on the `Minimized`/`Restored` EVENT - never paused.
2. **`close()` must reach the loop as an event too.** The loop ends on `Close` and nothing else, so a
   close that only set `closed_` left a paused loop running forever.
3. **`stop_host` is teardown only** (`surfaceDestroyed`/`nativeDestroy`); `onPause` calls `pause_host`.

`onResume` runs **before** `surfaceCreated` and the borrowed window is stale then, so
`resume_host` does not depend on the surface callback.

Verified: type `abc`, Home, return, tap, type `xyz` - the field reads `abcxyz`. Repeated four
times, the log shows `window hidden` then a clean `render thread exited`, no detach and no leak.

### 3.20 Folding the device: a rebuilt host must repaint

A fold is not a background cycle (3.19): closing destroys the surface, so the host is genuinely torn
down and rebuilt. `nativeSurfaceChanged` already forwards the extent to `set_surface_extent`, so the
metrics and the `Resize` were right; the defect was the paint.

`FramePipeline::tick` clears the damage accumulator and derives `should_render` from the dirty nodes it
collects, so a frame exists only when something is dirty. App launch satisfies that once - the first
layout dirties every node - which is why the bug hid there. On a fold the new host wraps the shell's
**live** scene: every node is clean, so the first tick planned nothing, the loop parked and the window
stayed black. A tap dirtied a node and the UI appeared; that is the tell.

The fix is the invariant: **host construction latches a full repaint**. The constructor sets
`full_repaint_pending` and marks the loop dirty directly, because the `AppHost` owning
`request_full_repaint` does not exist yet at that point; `resume_host` latches one too, matching
`WindowHost::set_visible(true)` on desktop. `app_host_repaint_tests.cpp` pins the headless shape:
settle a scene under one host, let it die, build a second over the same tree, and the first frame is a
full repaint that emits commands while the next paints nothing.

Verified: launch, fold `CLOSED`, unfold `OPENED`, unlock - the scene repaints with no tap and the
counters survive. The emulator locks its own screen on a fold transition, so `locksettings set-disabled
true` (or `KEYCODE_WAKEUP` + `wm dismiss-keyguard`) is required; without it the "black screen" is the
lock screen, not the app.

## 4. Related

- [`testing.md`](testing.md) — presets, the test matrix, failure triage.
- [`../design/mobile-integration.md`](../design/mobile-integration.md) — the Android surface, input,
  IME, accessibility and manifest design (design only).
- [`../specs/os-events-v1.md`](../specs/os-events-v1.md) — the Android event mapping.
