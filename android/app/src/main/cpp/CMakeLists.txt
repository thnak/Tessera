# SPDX-License-Identifier: MIT

# The Android app shell's native library (issue #189). Gradle invokes this through
# `externalNativeBuild` in build.gradle.kts, so the whole engine is built by the same CMake project
# the desktop lanes use: one build system, one set of options.
#
# The entry point is the JNI bridge, not `android_main`: the shell is a Java activity
# (TesseraActivity + TesseraView) because the platform reports insets, cutouts and fold posture
# through Java APIs that have no NDK equivalent. Only these two TUs include <jni.h>: `jni_bridge.cpp`
# for the surface/metrics/touch/IME seams and `a11y_bridge.cpp` for the accessibility provider's
# pull-side queries.

add_library(tessera_android_app SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/jni_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ime_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/caret_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/field_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/a11y_bridge.cpp)

# The engine. tessera_app_window is the window driver (Vulkan device, swapchain, Batched2D, the
# AppHost loop); tessera_app is the platform-neutral host it drives. tessera_text_edit is the edit
# controller and its scene runtime: the shell owns the text field's buffer, so the Java
# `InputConnection` forwards callbacks into it instead of keeping a copy of the text.
target_link_libraries(tessera_android_app PRIVATE
    tessera_accessibility
    tessera_app_window
    tessera_app
    tessera_hal_window
    tessera_paint
    tessera_text_edit)

# The bridge includes android_motion_translation.hpp and android_a11y_mapping.hpp, which live with
# the other Android pure rules.
target_include_directories(tessera_android_app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

# libandroid provides ANativeWindow_fromSurface/release; liblog is the logcat writer; libEGL is what
# the NDK's native-window header pulls in on some NDK versions.
target_link_libraries(tessera_android_app PRIVATE
    android
    log
    EGL)

target_compile_features(tessera_android_app PRIVATE cxx_std_23)
