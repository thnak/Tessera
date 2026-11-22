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

**The metrics seam.** `hal::Window` gains `set_physical_insets` and `set_physical_cutouts`, which
take the platform's **physical** pixels. The backend converts them with the decode rules in
`android_metrics_translation.cpp`, so the bridge never needs to know the layout rules and the
conversion stays unit-tested on every lane. The types live in `window_desc.hpp` so `window.hpp`
stays inside its size budget.

### 3.7 What is still missing

- **Fold posture over JNI.** The decode, the HAL seam (`set_physical_fold`) and its tests are done,
  but the Java reader is blocked on a dependency decision: `FoldingFeature`/`WindowInfo` live in
  **`androidx.window`**, not in the platform SDK (`javap` on `android.jar` finds neither). Reading a
  fold therefore needs an AndroidX dependency in the shell, which is a decision for whoever owns the
  dependency policy rather than something to slip into this change. Until then the shell reports no
  fold, which is the same answer a non-foldable gives.
- **The accessibility bridge itself.** The role/action mapping
  (`android_a11y_mapping.cpp`) and its tests are done, but nothing builds an
  `AccessibilityNodeProvider` from a snapshot yet, so a screen reader sees an empty window. The
  engine's `A11ySnapshot` already carries everything the provider needs; the Java half is the
  remaining work.
- **The IME's long tail:** `setSelection`/`requestCursorUpdates` are not forwarded yet, and
  `performEditorAction` reports "not handled" because the edit controller has no Done/Search/Go
  concept. The compose/commit/delete path is wired.
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

## 4. Related

- [`testing.md`](testing.md) — presets, the test matrix, failure triage.
- [`../design/mobile-integration.md`](../design/mobile-integration.md) — the Android surface, input,
  IME, accessibility and manifest design (design only).
- [`../specs/os-events-v1.md`](../specs/os-events-v1.md) — the Android event mapping.
