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
| `android/app/src/main/AndroidManifest.xml` | `NativeActivity`, the Vulkan feature requirements, and the `configChanges` contract |
| `android/app/src/main/cpp/android_main.cpp` | `android_main`: waits for the OS to hand over an `ANativeWindow`, then creates the host |
| `android/app/src/main/cpp/scene.hpp` | the shell's scene and stylesheet: a card, a tap counter and a text field |
| `android/app/src/main/cpp/CMakeLists.txt` | the `tessera_android_app` shared library, linking `tessera_app_window` |
| `android/app/build.gradle.kts` | compiles the engine through the same CMake project and packages the APK |

The scene outlives the host: `android_main` creates it once and creates/destroys the host around it
as surfaces come and go, so a backgrounded app keeps its state (the tap counter survives a
background/foreground cycle). The host borrows the root and owns the compiled stylesheet, which is
why the scene is a `unique_ptr` the shell holds rather than a local.

The manifest declares only what the engine can honour. `localeConfig`, `networkSecurityConfig`,
`enableOnBackInvokedCallback` and `POST_NOTIFICATIONS` are deliberately absent: declaring a
capability the app cannot honour routes platform events to a handler that does not exist.

`android_main` creates the host on `APP_CMD_INIT_WINDOW` and destroys it on `APP_CMD_TERM_WINDOW`,
so the host is gone before the window can be reused — the surface-lifecycle rule from
`docs/design/mobile-integration.md` section 3.1. It does not run the frame loop on the glue thread:
`WindowHost` is single-threaded and must be created, run and destroyed on the thread that owns the
window.

### 3.4 Input translation

`src/platform/android/android_input.cpp` translates `AInputEvent`s into `ui::InputEvent` and pushes
them into the queue `set_input_queue` installed. The shell forwards from `onInputEvent` through
`hal::Window::translate_input`, which takes an opaque `const void*` so no platform type reaches a
public header.

The mapping rules live in `android_input_translation.cpp`, which **includes no NDK header**: the
action, tool-type, source and keycode constants are stable ABI values, so the rules are unit-tested
on every lane (`android_input`). That is where the defects that a device lane cannot see cheaply are
caught — the scroll sign, the pointer-index packing, the cancel semantics.

Three rules worth knowing:

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

`src/platform/android/android_metrics_translation.cpp` decodes the platform's window-metrics signals
into `hal::WindowMetrics`. Like the input rules it **includes no Android header**, so it is
unit-tested on every lane (`android_metrics`).

The platform reports insets in **physical pixels**; `WindowMetrics` carries logical pixels. That
conversion is the whole reason this is a separate testable unit: a wrong scale turns a 24 px status
bar into a 24 dp one, which is a layout bug that only shows on a device with a non-1.0 density. The
decode **rounds** rather than truncates, so a 2.75× device does not lose a pixel per side.

Three rules worth knowing:

- **Cutout rectangles.** Android reports `right`/`bottom` as exclusive edges; the engine's rectangles
  are `x`/`y`/`width`/`height`, so the extent is the difference. A zero-area rectangle is dropped
  rather than reported as a cutout that insets nothing.
- **A flat fold does not occlude.** A flat fold is a seam, not a barrier: it reports `FoldState::Flat`
  and its axis, but `FoldOcclusion::None`. Only `HalfOpened` reports an occluding hinge, and only
  when the platform gives it a non-zero rectangle.
- **Orientation comes from the extent**, not from Android's rotation, so a device that reports
  neither still gets the same answer as the desktop lanes.

The window backend exposes `set_insets`, `set_cutouts` and `set_fold` for the host to call from its
platform callbacks; each queues at most one coalesced `SafeAreaChanged`/`FoldChanged` event, so a
burst of callbacks between two ticks costs one event per kind.

### 3.6 The JNI bridge

The shell is a **Java activity**, not `android.app.NativeActivity`, because the platform reports
insets, cutouts and fold posture through Java APIs that have no NDK equivalent. A NativeActivity can
hand the engine an `ANativeWindow` but cannot read those.

| File | Role |
|---|---|
| `android/app/src/main/java/dev/tessera/app/TesseraActivity.java` | the activity: edge-to-edge, hides the system bars, owns the view |
| `android/app/src/main/java/dev/tessera/app/TesseraView.java` | a `SurfaceView`: surface lifecycle, insets/cutouts, touch |
| `android/app/src/main/cpp/jni_bridge.cpp` | the only TU that includes `<jni.h>` |

`TesseraView` is a `SurfaceView` because that is the one view whose `Surface` the NDK can turn into
an `ANativeWindow` (`ANativeWindow_fromSurface`). A `TextureView` would give a `SurfaceTexture`,
which needs a different Vulkan path.

**The render thread.** `WindowHost` is single-threaded and must be created, run and destroyed on the
thread that owns the window, so the bridge starts a dedicated render thread that calls
`host->run(0)`. The Java thread only signals it: `surfaceDestroyed` closes the window, which makes
`run(0)` return, and then joins. `pump_once` on the Java thread would need a spin, and a spin on a
phone burns battery for nothing.

**The input seam.** The shell is a Java activity, so touch arrives as an `android.view.MotionEvent`,
not as the NDK's `AInputEvent`. The bridge reads the getters once into a `MotionSample` and the pure
rules in `android_motion_translation.cpp` do the mapping — the same split as the native path, and for
the same reason: the rules are plain numbers, so the physical-to-logical conversion, the scroll sign
and the pointer bookkeeping are unit-tested on every lane (`android_motion`) instead of only on a
device.

Two seams carry it. `hal::Window::post_input_event` queues an event the HOST translated;
`translate_input` is the opposite split, where the backend translates an opaque native event. Android
has both paths because the same platform can deliver touch either way.

**The IME bridge.** A custom-rendered text field does not get an IME for free: the platform talks to
an `InputConnection`, and the engine's edit controller consumes `ui::InputEvent`.
`TesseraInputConnection` is the adapter — it holds no text of its own, because the engine owns the
buffer, the undo history and the selection, and a copy in Java would be a second source of truth that
drifts.

Two other hooks are required, and both are easy to miss:

- `onCheckIsTextEditor()` must return true, or the platform never offers the view a keyboard.
- The show request is explicit (`InputMethodManager.showSoftInput`), because a `SurfaceView` cannot
  focus itself the way an `EditText` does.

The mapping rules live in `android_ime_translation.cpp` and are **pure over UTF-16** — no SDK, no
JNI — so `android_ime` pins the two conversions that fail silently on screen:

- **UTF-16 to UTF-8.** A naive per-character loop encodes each surrogate half separately and turns
  every emoji into two replacement characters.
- **UTF-16 offsets to UTF-8 byte offsets.** Android reports the caret in UTF-16 units; the engine's
  selection is UTF-8 bytes. Mixing them puts the caret in the wrong place on any non-ASCII string.

A lone surrogate becomes U+FFFD rather than being dropped, because dropping it would desynchronise
every offset after it.

**The engine owns the buffer, so the platform must ask.** Forwarding callbacks is only half an IME
bridge: the platform also *queries* the text, and a bridge that answers `""` makes the IME behave as
if the field were always empty. That is not cosmetic — suggestions stop, Backspace deletes nothing,
and the IME places its composing region at offset 0. So `TesseraInputConnection` overrides the
queries too, and each one reads the engine's buffer back:

| callback | what it reads |
|---|---|
| `getTextBeforeCursor` / `getTextAfterCursor` | the buffer around the caret, in UTF-16 units |
| `getSelectedText` | `EditBuffer::selected_text()` |
| `setSelection` | `EditBuffer::set_selection`, converting UTF-16 offsets to UTF-8 bytes |
| `deleteSurroundingTextInCodePoints` | the same delete path as the UTF-16 variant |

The shell owns one `EditController` and one `TextInputRuntime` (`Shell::editor`,
`Shell::editor_runtime`). The runtime is attached to the field's node, so a committed edit publishes
the buffer's text **and** the caret into that node's `PaintData` — a field whose node started with an
empty payload would otherwise paint nothing while the buffer filled up, leaving the caret moving over
blank space.

**The two enums are not the same enum.** `ui::InputEventType` is the normalized input vocabulary and
`ui::EventType` is the scene's; they do not share values (`EditText` is 13 in one and 14 in the
other). A `static_cast` between them silently turns a commit into a composition start, which looks
like "typing does nothing" while the buffer stays empty. The bridge maps the six types explicitly and
drops anything else.

**The metrics seam.** `hal::Window` gains `set_physical_insets` and `set_physical_cutouts`, which
take the platform's **physical** pixels. The backend converts them with the decode rules in
`android_metrics_translation.cpp`, so the bridge never needs to know the layout rules and the
conversion stays unit-tested on every lane. The types live in `window_desc.hpp` so `window.hpp`
stays inside its size budget.

### 3.7 What is still missing

- **Fold posture over JNI.** Done — see section 3.10. The decode, the HAL seam
  (`set_physical_fold`), the Java reader and the live observer are all wired.
- **The accessibility bridge itself.** The role/action mapping
  (`android_a11y_mapping.cpp`), the pull-side provider (`snapshot_provider.cpp`) and the Java
  `AccessibilityNodeProvider` are done — see section 3.9. What is not wired: live-region
  announcements, collection set-size/position, and `SetText`/`SetSelection` actions (they are offered
  to the platform but the edit controller drops them).
- **The IME's long tail:** `requestCursorUpdates` is not forwarded (the engine has no cursor-anchor
  report), and `performEditorAction` reports "not handled" because the edit controller has no
  Done/Search/Go concept. The compose/commit/delete path and the text queries are wired.
- **Selection and the context menu.** A tap now places the caret (section 3.13), but a long-press
  does not start a selection, there is no drag-to-select, and there is no context menu or clipboard
  backend for Android (`src/platform/windows/clipboard_win32.cpp` is the only implementation;
  `clipboard_unsupported.cpp` is the fallback). `EditController::extend_selection_to` is the seam a
  long-press would use; nothing calls it yet.
- **A generated component.** The shell's scene (`android/app/src/main/cpp/scene.hpp`) is built by
  hand, because the codegen CLI runs on the host at build time and `tessera_compiler_demo` is gated
  on `NOT CMAKE_CROSSCOMPILING`. A hand-built tree still exercises the scene graph, the compiled
  stylesheet, Yoga layout, Batched2D and the input path (a Button that counts taps, a TextInput).
  Replacing it with the generated `demo::Demo` needs the codegen host tool — the same
  `ExternalProject` pattern the shader manifest checker uses.

### 3.8 Emulator GPU mode matters

| Emulator GPU mode | Device | API version | Tessera accepts? |
|---|---|---|---|
| `-gpu swiftshader_indirect` (the CI default) | SwiftShader | 1.2.0 | **No** — `choose_device` requires 1.3 |
| `-gpu host` | the host GPU (passthrough) | 1.3.0+ | Yes |

A lane that wants to exercise the real renderer must boot with `-gpu host`; the default software
renderer is below the minimum the device-selection policy enforces
(`src/rendering/backend/vulkan/src/vulkan_device_selection.hpp`).

### 3.9 Accessibility: the one platform that pulls instead of pushing

Windows (UIA) and Linux (AT-SPI) are **push** bridges: a bridge thread on the app side calls out, and
the app answers from a tree it already published. Android is the opposite. `AccessibilityNodeProvider`
is **pull**: the platform calls `createAccessibilityNodeInfo(virtualViewId)` on the UI thread and
expects that one node back before the call returns. A bridge shaped like UIA's would have to block
the platform on a frame, which is exactly what the no-blocking rule forbids.

So the Android column is a different shape, not a branch in an existing one:

- **`ui::a11y::SnapshotProvider`** keeps the last published snapshot and answers one node at a time.
  Its mutex covers one bounded copy (at most `kMaxNodes` entries), never a snapshot build, so the
  render thread and the platform thread never wait on each other for longer than that copy.
- **`AccessibilityBackend::Provider`** is the driver's third backend. It reuses the controller's
  existing dirty tracking and action drain, so a static window still builds no tree and publishes
  nothing. The shell sets `WindowOptions::accessibility_provider`; a null provider reports
  `UnsupportedPlatform`, which is the documented "accessibility off, window keeps running" path.
- **`TesseraAccessibilityProvider`** (Java) is what the platform talks to. It holds **no tree**: each
  query is one JNI call returning the node's class name, text, description, flags, bounds, action ids
  and child ids, and it copies them into an `AccessibilityNodeInfo`.

Three details that are easy to get wrong, and that the tests pin:

**Virtual ids must be stable.** A snapshot index is a pre-order position, so it renumbers whenever
the tree is rebuilt. A screen reader that focused virtual view 3 would then be pointing at a
different control. `SnapshotProvider` keeps a slot table: a scene node keeps its virtual id for as
long as it stays in the tree, and a node that leaves frees its slot. Id `0` is the host view, so a
real node is always `1..kMaxNodes`.

**The action ids are the platform's, not ours.** They are *not* consecutive powers of two:
`ACTION_FOCUS` is 1, `ACTION_CLICK` is 16, `ACTION_SCROLL_FORWARD` is 4096, `ACTION_SET_TEXT` is
2097152. Numbering them in enum order turns a click into a focus request, and because the platform
echoes back the id it was offered, that failure is silent. The values in `android_a11y_mapping.hpp`
were read from `android.jar` with `javap -constants` and the test asserts them. They are also a
**list**, not a bit set: `Click | LongClick` would be 48, which is not an id the platform knows.

**Bounds are the engine's logical pixels.** They are the view's own coordinate space, so no density
conversion happens in Java. They cross as fixed point in 1/64 px so a fractional logical position is
rounded once rather than truncated twice.

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

**The platform has no fold API.** Verified against the build's own `android.jar` for API 35, 36.1 and
37: no class matches fold, hinge or posture, and `android.view.WindowInfo` does not exist at any of
those levels. `android.view.DisplayShape` does exist but describes the outline of a display, not a
hinge, so it cannot substitute.

Posture is exposed only by **Jetpack WindowManager** (`androidx.window:window`), Google's official
Apache-2.0 library for exactly this. It was already the planned dependency:
[`dependencies.md`](../specs/dependencies.md) lists it as "Android V1 optional" and
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) already carried the entry, so this slice
consumes a decision that was already made rather than making a new one.

The shell pins **1.5.1** (current stable) and uses it for one call. The chain it pulls in is
`window-core`, `androidx.core`, `androidx.collection`, `androidx.annotation`, `jspecify`,
`kotlin-stdlib` and `kotlinx-coroutines-android` — which is why the build also enables the Kotlin
plugin, for one 40-line file.

**Why a Kotlin file.** `WindowInfoTracker.windowLayoutInfo` returns a Kotlin `Flow`. Collecting one
from Java means calling suspend functions through `BuildersKt` with a hand-written `Function2`, which
compiles only while every parameter type lines up and breaks on any coroutines version bump. I tried
that first and it failed to compile; `FoldObserver.kt` does it in three lines instead. All the mapping
stays in `TesseraFoldReader.java` with the rest of the shell.

**Why a listener, not just a config-change re-read.** A fold change is *not* always a configuration
change. The emulator's `HALF_OPENED` transition changes the hinge angle without resizing the window,
so `onConfigurationChanged` never fires — measured, not assumed. A re-read on that hook alone reports
the posture the app started with, forever.

**The ordering trap.** `onResume` runs *before* `surfaceCreated`, so a fold read there has no native
handle to push to. `TesseraView.whenNativeReady` defers the first push until the shell exists;
without it a device that is already folded at launch reports no fold until the next change, which may
never come.

Verification on the emulator, which can simulate a foldable:

```powershell
adb shell cmd device_state print-states   # lists CLOSED, HALF_OPENED, OPENED, ...
adb shell cmd device_state state 1        # HALF_OPENED
adb shell cmd device_state state 2        # OPENED
```

| Device state | posture | axis | hinge |
|---|---|---|---|
| `OPENED` | 1 (Flat) | 2 (Vertical) | `1038,0,1038,2152` |
| `HALF_OPENED` | 2 (HalfOpened) | 2 (Vertical) | `1038,0,1038,2152` |

The hinge sits at x=1038, the exact centre of the 2076 px display, and the transition is observed
live without restarting the activity.

### 3.11 Hinge angle: the one fold signal the platform SDK does have

Posture is discrete and needs AndroidX. The **angle** is continuous and does not: the platform
exposes it as a sensor, `android.hardware.Sensor.TYPE_HINGE_ANGLE` (36), present in `android.jar`
for API 35 and later and in the NDK as `ASENSOR_TYPE_HINGE_ANGLE`. So this slice adds no dependency
— it is `SensorManager`, which every Android build already has.

**Why it is a separate reader, not part of `TesseraFoldReader`.** The two read different sources at
different rates, and a device can have one without the other:

| | posture | angle |
|---|---|---|
| source | `FoldingFeature` (AndroidX) | `SensorManager` (platform) |
| values | `FLAT`, `HALF_OPENED` | 0–360 degrees |
| rate | one event per transition | continuous while folding |
| absent when | no foldable | no hinge sensor |

A foldable with no hinge sensor still reports posture; a device whose window is not laid out yet
reports no posture while the sensor keeps working. Folding them into one reader would make each
depend on the other's failure mode.

**Why a separate event type.** `HingeAngleChanged` is not `FoldChanged`. A posture change is a
discrete relayout; the angle is a continuous signal a scene may animate against. Merging them would
rebuild the layout on every degree of a slow fold. The angle event coalesces into the newest, so a
burst of sensor samples between two ticks costs one event, and it maps onto the taxonomy's
`PostureChanged` (`FoldChanged` stays the discrete transition).

**The sentinel is negative.** `kUnknownHingeAngle` is `-1`, not `0`, because `0` is a real
measurement — a fully closed hinge. A sentinel of `0` would make "no sensor" indistinguishable from
"closed". The decode (`android::hinge_angle`) also turns a non-finite reading into the sentinel
rather than clamping it to `0`, because a broken HAL reporting `NaN` must not look like a closed
device. The upper clamp is 360, not 180: a display that folds all the way around is real.

**The angle survives an unknown posture.** `fold_info` sets the angle before the posture switch, so
a device that has a sensor but no `FoldingFeature` still reports its angle. Dropping it there would
make a foldable look like a slab for as long as the posture is unknown.

**Registration follows the lifecycle.** The sensor is registered on resume and released on pause.
It is a non-wake-up sensor, so leaving it registered while backgrounded would keep it powered for a
value no frame can show.

Verification on the emulator, which has a hinge sensor:

```powershell
adb shell dumpsys sensorservice | Select-String "hinge"
# 0x0000000b) Goldfish hinge sensor0 (in degrees)
#   type: android.sensor.hinge_angle(36)
#   last events: 90.00, 0.00
```

The emulator reports `90.00` at `HALF_OPENED` and `0.00` at `CLOSED`, which is the sensor's own
scale (0 closed, 180 flat) and not the posture enum.

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

Two rules matter there:

- A key-down that types a character produces **two** events, the key event and a `Text` event. The
  edit controller inserts from the latter; a bridge that forwards only the key event types nothing.
- A key the engine has no logical code for produces **nothing**, so the caller lets the platform
  handle it. That is what keeps the system Back gesture working; consuming it would break navigation.

An IME that synthesises a key (Space, Enter) does **not** go through the view's dispatcher: the IME
owns the input connection, so the event arrives at `TesseraInputConnection.sendKeyEvent` instead.
That override takes the same `nativeKey` path, which is why Space types at all — without it the
field could not contain a space while letters (committed as text) still worked.

Enter has two paths and both are needed. A hardware Enter arrives as a key event; a soft-keyboard
Enter on a multi-line field arrives as `performEditorAction`, which the connection answers by
sending the same key pair. A single-line field gates the newline off by design (submit is a form
concern the engine defers), so the shell's demo field is multi-line — otherwise Enter reads as a
broken key on a phone with no form to submit.

`adb shell input keyevent` is not a substitute for a real keyboard here: it delivers only the
key-up to the view (the default `View.onKeyDown` consumes the down), so a newline cannot be
reproduced with it. Tap the soft keyboard's Enter instead.

**The caret.** `CaretBlink` is host-owned (`WindowHost::Impl::caret`) and ticks once per driver
iteration, but the host cannot know whether a field is focused — only the app can. Two options close
the loop:

| option | direction | why |
|---|---|---|
| `caret_active` | app → host | the app's focus/editability policy, asked every iteration |
| `caret_target` | host → app | the `TextInputRuntime` whose decoration carries the caret |

Without `caret_active` the blink never starts; without `caret_target` it starts but nothing it
decides reaches `TextDecoration::blink_phase`, so paint draws a solid caret forever. Both are null by
default, which keeps every desktop caller on the pre-blink behaviour. `caret_blink_target` (a GPU
test) pins the round trip: focus through the policy alone publishes a visible phase, a half-period
flip publishes a hidden one, and blurring publishes hidden immediately.

### 3.13 A tap places the caret

A custom-rendered field does not get caret placement for free either. The platform reports a touch
as a point in view coordinates; the edit controller wants a `TextHit` (a UTF-8 byte offset plus a
caret affinity). `hit_test_point` already does the hard part — line selection by y, then x inside
that line's visual runs, bidi-aware and snapped to a grapheme boundary — but it takes a `TextStyle`,
a `TextConstraints` and a `FontMetrics`, and the bridge has none of those.

`android_caret_translation.cpp` is the adapter, and it is **pure**: no NDK, no JNI, no device. The
box arithmetic is where a caret lands one character off, so `android_caret` pins it on every lane:

| rule | why it bites |
|---|---|
| physical pixels ÷ density, then minus the box origin | the layout is logical; skipping the divide puts every caret at the wrong character on a 2.4x phone |
| the point is measured from the **content** box | the text starts inside the padding, so a tap in the padding must clamp, not resolve to a negative offset |
| a point outside the border box is `inside == false` | a tap elsewhere must leave the caret alone, not jump it to the start |
| a zero density is treated as 1 | the platform can report an unknown density, and dividing by it would make every point NaN |

Two things the bridge must get right, and both are easy to get wrong:

- **The density is already applied.** `translate_motion_sample` divides the platform's physical
  pixels by the density, so the event the bridge sees is in logical pixels. Passing the density to
  `caret_from_point` again divides twice — the first version of this slice did exactly that, and the
  caret did not move at all.
- **The hit test must measure the glyphs paint draws.** The shell loads the same face paint uses
  (`/system/fonts/Roboto-Regular.ttf`) and keeps a `FreeTypeFontMetricsProvider`, rather than
  guessing metrics. A face that fails to load leaves the provider null and the tap declines.

`caret_bridge.cpp` holds the shell half (the field's box, the blink policy, the placement call) so
`jni_bridge.cpp` stays inside its size budget. What is still missing is the other half of the
gesture: a long-press does not start a selection and there is no drag-to-select.

## 4. Related

- [`testing.md`](testing.md) — presets, the test matrix, failure triage.
- [`../design/mobile-integration.md`](../design/mobile-integration.md) — the Android surface, input,
  IME, accessibility and manifest design (design only).
- [`../specs/os-events-v1.md`](../specs/os-events-v1.md) — the Android event mapping.
