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
}

android {
    namespace = "dev.tessera.app"
    compileSdk = 35
    ndkVersion = "30.0.16248370"

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
            path = file("../../../../CMakeLists.txt")
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
