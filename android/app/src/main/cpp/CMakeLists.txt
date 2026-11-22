# SPDX-License-Identifier: MIT

# The Android app shell's native library (issue #189). Gradle invokes this through
# `externalNativeBuild` in build.gradle.kts, so the whole engine is built by the same CMake project
# the desktop lanes use: one build system, one set of options.
#
# The entry point is the JNI bridge, not `android_main`: the shell is a Java activity
# (TesseraActivity + TesseraView) because the platform reports insets, cutouts and fold posture
# through Java APIs that have no NDK equivalent. The bridge is the only TU that includes <jni.h>.

add_library(tessera_android_app SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/jni_bridge.cpp)

# The engine. tessera_app_window is the window driver (Vulkan device, swapchain, Batched2D, the
# AppHost loop); tessera_app is the platform-neutral host it drives.
target_link_libraries(tessera_android_app PRIVATE
    tessera_app_window
    tessera_app
    tessera_hal_window)

# libandroid provides ANativeWindow_fromSurface/release; liblog is the logcat writer; libEGL is what
# the NDK's native-window header pulls in on some NDK versions.
target_link_libraries(tessera_android_app PRIVATE
    android
    log
    EGL)

target_compile_features(tessera_android_app PRIVATE cxx_std_23)
