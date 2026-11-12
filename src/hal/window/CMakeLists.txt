# SPDX-License-Identifier: MIT

# The clipboard HAL target lives in src/hal/clipboard; it is registered here until the root
# CMakeLists (Lead-owned) lists that directory directly. Moving the add_subdirectory line later is a
# no-op for the target itself.
add_subdirectory(${PROJECT_SOURCE_DIR}/src/hal/clipboard ${CMAKE_BINARY_DIR}/src/hal/clipboard)

add_library(tessera_hal_window STATIC)

# The pure display-refresh value rules (validation, "unknown", explicit-option precedence and the
# X11 mode math) live here on every platform, so the platform files only read the OS value and hand
# it over. Tests compile this file directly, without a window or a display.
target_sources(tessera_hal_window PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/refresh_rate.cpp)

# Private shared implementation header (normalized event ring).
target_include_directories(tessera_hal_window PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

if (WIN32)
    target_sources(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_accessibility.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/win32_ime_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/win32_input.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/win32_input_ime.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/win32_pointer_translation.cpp)
    target_include_directories(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows)
    # Input is translated straight into the application's ui::InputQueue. imm32 provides the IME
    # composition-string queries; uiautomationcore answers WM_GETOBJECT with the accessibility
    # bridge's root provider (UiaReturnRawElementProvider).
    target_link_libraries(tessera_hal_window PUBLIC user32 PRIVATE tessera_input imm32
        uiautomationcore)
elseif (TESSERA_X11_FOUND)
    target_sources(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/linux/window.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_input.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_input_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_ime_composition.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2_events.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2_ime.cpp)
    target_include_directories(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/linux)
    target_link_libraries(tessera_hal_window PUBLIC X11::X11 PRIVATE tessera_input)
    # XInput2 (XI 2.1 smooth-scroll valuators, XI 2.2 touch, tablet pressure) needs the XInput2
    # headers and libXi. Without them the browser-independent pure rules still compile and the
    # window keeps the core-protocol pointer path unchanged (TESSERA_X11_XI2 undefined).
    find_path(TESSERA_XINPUT2_INCLUDE_DIR X11/extensions/XInput2.h)
    find_library(TESSERA_XINPUT2_LIBRARY NAMES Xi)
    if (TESSERA_XINPUT2_INCLUDE_DIR AND TESSERA_XINPUT2_LIBRARY)
        target_compile_definitions(tessera_hal_window PRIVATE TESSERA_X11_XI2=1)
        target_link_libraries(tessera_hal_window PRIVATE ${TESSERA_XINPUT2_LIBRARY})
        message(STATUS "XInput2 found: Linux smooth scroll, touch and pen input enabled")
    else()
        message(STATUS "XInput2 not found: Linux input keeps the core-protocol path")
    endif()
    # The display-refresh query reads the active CRTC mode through RandR. Without the headers or
    # the library the window compiles the documented "unknown" path instead (no divisor exists),
    # which is also what a server without the extension reports at runtime.
    find_path(TESSERA_XRANDR_INCLUDE_DIR X11/extensions/Xrandr.h)
    find_library(TESSERA_XRANDR_LIBRARY NAMES Xrandr)
    if (TESSERA_XRANDR_INCLUDE_DIR AND TESSERA_XRANDR_LIBRARY)
        target_sources(tessera_hal_window PRIVATE
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_randr.cpp)
        target_compile_definitions(tessera_hal_window PRIVATE TESSERA_X11_RANDR=1)
        target_link_libraries(tessera_hal_window PRIVATE ${TESSERA_XRANDR_LIBRARY})
        message(STATUS "Xrandr found: Linux display-refresh query enabled")
    else()
        message(STATUS "Xrandr not found: Linux reports the unknown display refresh")
    endif()
else()
    target_sources(tessera_hal_window PRIVATE
        window_stub.cpp)
endif()

target_include_directories(tessera_hal_window PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# The base Window fallback and the public window_wait.hpp helper use the HAL steady clock.
target_link_libraries(tessera_hal_window PUBLIC tessera_hal_timer)

target_compile_features(tessera_hal_window PUBLIC cxx_std_23)
