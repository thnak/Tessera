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

## 3. Vulkan does not cross-compile yet

`-DTESSERA_BUILD_VULKAN=ON` configures — it finds the NDK's `libvulkan.so` — but the build fails:
the shader rules run `tessera_shader_manifest_check` as a build-time command, and that executable is
built for the **target**, so the host cannot execute it (`'tessera_shader_manifest_check' is not
recognized`). A Vulkan Android build needs that tool built for the host first, the same
host-tools pattern the codegen fixtures use. Until then the null backend is the supported Android
configuration.

The emulator does expose Vulkan (`android.hardware.vulkan.version` reports 1.3.0), so once the
host-tool gap is closed the surface path is testable there.

## 4. Related

- [`testing.md`](testing.md) — presets, the test matrix, failure triage.
- [`../design/mobile-integration.md`](../design/mobile-integration.md) — the Android surface, input,
  IME, accessibility and manifest design (design only).
- [`../specs/os-events-v1.md`](../specs/os-events-v1.md) — the Android event mapping.
