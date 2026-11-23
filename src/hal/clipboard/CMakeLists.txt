# SPDX-License-Identifier: MIT

add_library(tessera_hal_clipboard STATIC
    clipboard.cpp)

if (WIN32)
    target_sources(tessera_hal_clipboard PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows/clipboard_win32.cpp)
    target_link_libraries(tessera_hal_clipboard PUBLIC user32)
elseif (ANDROID)
    # The Android backend (issue #189, android-lane.md section 3.16). `ClipboardManager` must be
    # touched on a thread with a `Looper`, and the engine's render thread has none, so the backend
    # keeps a cache the UI thread refreshes. The UTF-8/UTF-16 conversion is in
    # android_clipboard_text.cpp, which includes no Android header and is unit-tested on every lane.
    target_sources(tessera_hal_clipboard PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/android/clipboard_android.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_clipboard_text.cpp)
    target_include_directories(tessera_hal_clipboard PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/android)
else()
    target_sources(tessera_hal_clipboard PRIVATE
        clipboard_unsupported.cpp)
endif()

target_include_directories(tessera_hal_clipboard PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_hal_clipboard PUBLIC cxx_std_23)
