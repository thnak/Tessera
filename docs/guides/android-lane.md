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
| `android/app/src/main/cpp/CMakeLists.txt` | the `tessera_android_app` shared library, linking `tessera_app_window` |
| `android/app/build.gradle.kts` | compiles the engine through the same CMake project and packages the APK |

The manifest declares only what the engine can honour. `localeConfig`, `networkSecurityConfig`,
`enableOnBackInvokedCallback` and `POST_NOTIFICATIONS` are deliberately absent: declaring a
capability the app cannot honour routes platform events to a handler that does not exist.

`android_main` creates the host on `APP_CMD_INIT_WINDOW` and destroys it on `APP_CMD_TERM_WINDOW`,
so the host is gone before the window can be reused — the surface-lifecycle rule from
`docs/design/mobile-integration.md` section 3.1. It does not run the frame loop on the glue thread:
`WindowHost` is single-threaded and must be created, run and destroyed on the thread that owns the
window.

### 3.4 What is still missing

- **Input.** `AInputEvent` arrives in `android_main` and is dropped (`onInputEvent` returns 0, so the
  platform falls back to its own behaviour). Translating `MotionEvent`/`KeyEvent` into
  `ui::InputEvent` is `docs/design/mobile-integration.md` section 3.2.
- **Insets, cutouts, fold.** These need `WindowInsets`/`WindowMetricsCalculator` over JNI
  (sections 3.5 and 3.6); the metrics carry the documented zero-inset fallback.
- **IME and accessibility** (sections 3.3 and 3.4).
- **A scene.** `android_main` creates the host with no scene or stylesheet, so it reports
  unavailable. A real app supplies them where the shell has the comment.

### 3.5 Emulator GPU mode matters

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
