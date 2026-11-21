# SPDX-License-Identifier: MIT

# The Android app shell's native library (issue #189). Gradle invokes this through
# `externalNativeBuild` in build.gradle.kts, so the whole engine is built by the same CMake project
# the desktop lanes use: one build system, one set of options.
#
# `android_native_app_glue` ships with the NDK as a source file, not a library, so it is compiled
# into this target. That is the documented way to use it and avoids depending on a prebuilt that
# may not match the NDK version.

add_library(tessera_android_app SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/android_main.cpp
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(tessera_android_app PRIVATE
    ${ANDROID_NDK}/sources/android/native_app_glue)

# The engine. tessera_app_window is the window driver (Vulkan device, swapchain, Batched2D, the
# AppHost loop); tessera_app is the platform-neutral host it drives.
target_link_libraries(tessera_android_app PRIVATE
    tessera_app_window
    tessera_app
    tessera_hal_window)

# android_native_app_glue needs libandroid, liblog and libEGL (for ANativeWindow_fromSurface).
target_link_libraries(tessera_android_app PRIVATE
    android
    log
    EGL)

target_compile_features(tessera_android_app PRIVATE cxx_std_23)

# The glue source is C, and the NDK's copy is not warning-clean under -Werror. It is third-party
# code, so it gets the same narrow exemption the vendored libraries do.
set_source_files_properties(
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
    PROPERTIES COMPILE_OPTIONS "-w")
