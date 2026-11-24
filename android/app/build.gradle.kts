// SPDX-License-Identifier: MIT

// The Android app shell's Gradle build (issue #189).
//
// It does one thing: compile the engine through the same CMake project the desktop lanes use, and
// package the resulting .so into an APK. There is no Java or Kotlin source: the activity is
// android.app.NativeActivity, which the platform provides, and the engine's entry point is
// android_main in cpp/android_main.cpp.
//
// The NDK version is pinned to the one the CI Android lane uses. An older NDK does not compile the
// tree at all: NDK 28's libc++ has no floating-point std::from_chars, which src/harness/json_parse.cpp
// needs (docs/guides/android-lane.md section 1).

plugins {
    id("com.android.application") version "8.7.0"
    // Kotlin is needed for exactly one file (FoldObserver.kt): the fold tracker's only continuous
    // API is a Kotlin Flow, and collecting one from Java means calling suspend functions through
    // BuildersKt with a hand-written Function2, which breaks on any coroutines version bump.
    // androidx.window already puts Kotlin on the classpath, so this adds no new runtime dependency.
    id("org.jetbrains.kotlin.android") version "2.0.21"
}

android {
    namespace = "dev.tessera.app"
    compileSdk = 35
    ndkVersion = "30.0.16248370"

    // Kotlin and Java must agree on a JVM target or Gradle refuses the build ("Inconsistent
    // JVM-target compatibility"). 17 is the highest the Android toolchain accepts for both without
    // a toolchain download, and nothing here needs newer bytecode.
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    defaultConfig {
        applicationId = "dev.tessera.app"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "0.0.1"

        // The engine is C++23 and links the static libc++ the NDK ships, so the APK carries no
        // shared C++ runtime the device would have to provide.
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DTESSERA_BUILD_TESTS=OFF",
                    "-DTESSERA_BUILD_VULKAN=ON",
                    "-DTESSERA_BUILD_POC=OFF",
                    "-DTESSERA_WARNINGS_AS_ERRORS=ON",
                    "-DANDROID_STL=c++_static",
                )
                cppFlags += "-std=c++23"
            }
        }

        ndk {
            // arm64-v8a is what ships; x86_64 is what the emulator runs. Both are built so the same
            // APK installs on a device and on the CI emulator.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            // The engine's own CMake project, not a copy: the app shell is a target inside it.
            // The path is resolved from the Gradle root (`android/`), so it is two levels up to the
            // repository root. A relative path here is relative to the MODULE directory
            // (`android/app`), which is why the earlier `../../../../` resolved outside the repo.
            path = file("../../CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    // The engine's CMake project builds the whole tree, including the tools and the test
    // executables' sources. Only the app target is packaged.
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    // The first Java test in the shell (issue #889). JUnit 4 only, and deliberately not Robolectric:
    // reading an `AccessibilityNodeInfo` needs an Android runtime, so anything that touches one is
    // untestable here without a framework this environment cannot fetch. `TesseraA11ySlots` is pure
    // arithmetic on constants, which is exactly the part that must agree with `a11y_bridge.cpp`.
    testImplementation("junit:junit:4.13.2")
    // Jetpack WindowManager is the ONLY way to read fold posture on Android: the platform SDK has no
    // fold, hinge or posture API at any level (verified against android.jar for API 35, 36.1 and 37;
    // android.view.WindowInfo does not exist either). Google publishes it as an official Jetpack
    // library under Apache-2.0, and it is what Google documents for foldables.
    //
    // 1.5.1 is the current stable release. It is used for exactly one call -
    // WindowInfoTracker.getCurrentWindowLayoutInfo - so the dependency is a leaf, not a framework.
    implementation("androidx.window:window:1.5.1")
}
