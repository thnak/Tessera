# SPDX-License-Identifier: MIT

# The clipboard HAL target lives in src/hal/clipboard; it is registered here until the root
# CMakeLists (Lead-owned) lists that directory directly. Moving the add_subdirectory line later is a
# no-op for the target itself.
add_subdirectory(${PROJECT_SOURCE_DIR}/src/hal/clipboard ${CMAKE_BINARY_DIR}/src/hal/clipboard)

# Native-surface seam (ADR-050 section 7, AF6, issue #178), same arrangement: registered here until
# the root CMakeLists lists src/hal/surface beside src/hal/window.

# The platform-NEUTRAL HAL value rules that the *style/layout* stack consumes. They are split into
# their own target because `tessera_hal_window` also carries the Win32/X11/Wayland backends, and a
# UI-layer consumer must never drag an OS window backend into a build that has none - the Wasm
# studio viewer is exactly such a consumer (issue #408: it resolves a simulated `FoldInfo` through
# `hal::resolve_safe_area`). Linking this small target instead keeps ADR-020's dependency direction
# (UI sees normalized types, never platform code) true by construction rather than by convention.
add_library(tessera_hal_metrics STATIC
    # SafeArea inset arithmetic (safe-area-v1.md sections 2 to 4): the source table, the cutout and
    # hinge insets, and the per-side resolution. Pure, allocation-free, no OS type.
    ${CMAKE_CURRENT_SOURCE_DIR}/safe_area.cpp
    # The simulated fold postures of the design-time studio (designer-studio-v1.md section 6,
    # issue #408): the ONE posture -> `FoldInfo` mapping the Wasm viewer, the native preview CLI and
    # `tessera-mcp` share, so a posture cannot mean two things in three places.
    ${CMAKE_CURRENT_SOURCE_DIR}/fold_posture.cpp
    # The normalized OsEvent taxonomy and the deterministic synthetic injector (issue #446): the
    # studio's `platform.event` feeds the injector in the Wasm viewer, which links no OS backend, so
    # these pure files live beside the other platform-neutral HAL rules instead of in the window
    # library. `tessera_hal_window` links this target, so every native consumer still sees them.
    ${CMAKE_CURRENT_SOURCE_DIR}/event_reason.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/display.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/os_event.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/window_events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/synthetic_events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/synthetic_system_buttons.cpp)

target_include_directories(tessera_hal_metrics PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# The injector's `feed_input` hands events to the pump's `ui::InputQueue`.
target_link_libraries(tessera_hal_metrics PUBLIC tessera_input)

target_compile_features(tessera_hal_metrics PUBLIC cxx_std_23)

# The simulated fold postures (issue #408): the posture -> `FoldInfo` mapping, the
# `FoldChanged`-equivalent and the rotation rule, against the SAME `resolve_safe_area` the app runs.
# Registered here rather than in tests/unit/CMakeLists.txt so this lane never edits the shared
# registry while other lanes work in it - the pattern `src/ui/style` and `src/compiler/codegen`
# already use.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(fold_posture_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/fold_posture_tests.cpp)
    target_link_libraries(fold_posture_tests PRIVATE tessera_hal_metrics)
    add_test(NAME fold_posture COMMAND fold_posture_tests)

    # The cross-thread wake (issue #842). Registered here, not in the shared unit registry, so this
    # lane never edits a file another lane is working in.
    add_executable(hal_wake_source_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/hal_wake_source_tests.cpp)
    target_link_libraries(hal_wake_source_tests PRIVATE tessera_hal_window)
    add_test(NAME hal_wake_source COMMAND hal_wake_source_tests)
endif ()

add_library(tessera_hal_window STATIC)

# The pure display-refresh value rules (validation, "unknown", explicit-option precedence and the
# X11 mode math) live here on every platform, so the platform files only read the OS value and hand
# it over. Tests compile this file directly, without a window or a display.
target_sources(tessera_hal_window PRIVATE
    # The cross-thread wake a parked UI thread joins (issue #842): an `eventfd` on POSIX, null
    # where the platform has no such primitive (the host then keeps its bounded sleep). Pure POSIX,
    # so it compiles on every non-Windows lane including the stub build.
    ${CMAKE_CURRENT_SOURCE_DIR}/wake_source_posix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/refresh_rate.cpp
    # The OS backdrop resolution table (docs/specs/glass-v1.md section 6): pure, platform-neutral,
    # so the platform files only detect capabilities and apply the returned material.
    ${CMAKE_CURRENT_SOURCE_DIR}/backdrop.cpp
    # Cutout list building (cap, bad-rect drop) and the deterministic metrics dump; platform-neutral.
    ${CMAKE_CURRENT_SOURCE_DIR}/cutout.cpp
    # Custom title-bar hit regions (window-chrome-v1.md section 8.1): the one resolver every backend
    # uses, so the non-client contract is tested without an OS window.
    ${CMAKE_CURRENT_SOURCE_DIR}/caption.cpp
    # Full-screen state machine (issue #232): enter/exit/restore bookkeeping shared by Win32 and X11.
    ${CMAKE_CURRENT_SOURCE_DIR}/fullscreen.cpp
    # The pointer-shape names (issue #191): pure, so the metrics dump and the diagnostics are
    # testable without a window or a display.
    ${CMAKE_CURRENT_SOURCE_DIR}/cursor.cpp
    # SafeArea inset arithmetic and the simulated fold postures live in `tessera_hal_metrics`
    # above, because the style module links them and must not pull an OS backend with them.
    # Event reasons, the OsEvent taxonomy and the synthetic injector (issue #446) are in
    # `tessera_hal_metrics` above; this target links it.
    # The OS appearance preferences (issue #187): the portal value/message decode and the GTK
    # settings-file fallback. Both pure, so the Linux feature's decisions are testable on a box with
    # no session bus. The normalized snapshot itself is `constexpr` in
    # `include/tessera/hal/appearance.hpp`, so the style engine can fold it into a `MediaContext`
    # without the UI layer linking this library (ADR-020's dependency direction).
    ${CMAKE_CURRENT_SOURCE_DIR}/portal_appearance.cpp)

# Private shared implementation header (normalized event ring).
target_include_directories(tessera_hal_window PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

if (WIN32)
    target_sources(tessera_hal_window PRIVATE        ${PROJECT_SOURCE_DIR}/src/platform/windows/window.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_accessibility.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_backdrop.cpp
        # Custom title-bar hit test and full-screen (issue #232), outside window.cpp's size budget.
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_caption_win.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_fullscreen_win.cpp
        # Pointer shapes (issue #191): the stock-cursor table and the WM_SETCURSOR answer.
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_cursor_win.cpp
        # Display capabilities (ADR-023): the per-monitor reads, the DPI/create-size arithmetic and
        # the display/settings message classification, in separate TUs so none exceeds its budget.
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_display_win.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_display_messages_win.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_dpi_win.cpp
        # OS appearance preferences (issue #187): SPI_GETCLIENTAREAANIMATION and AppsUseLightTheme,
        # in their own TU so neither window.cpp nor window_display_win.cpp grows.
        ${PROJECT_SOURCE_DIR}/src/platform/windows/window_appearance_win.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/win32_ime_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/win32_input.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/win32_input_ime.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/win32_pointer_translation.cpp)
    target_include_directories(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows)
    # Input is translated straight into the application's ui::InputQueue. imm32 provides the IME
    # composition-string queries; uiautomationcore answers WM_GETOBJECT with the accessibility
    # bridge's root provider (UiaReturnRawElementProvider). dwmapi owns the Windows 11 system
    # backdrop attribute and the frame extension of a translucent window (glass-v1.md section 6).
    # shcore supplies GetDpiForMonitor, the per-monitor scale query of the display-capability slice.
    target_link_libraries(tessera_hal_window PUBLIC user32 PRIVATE tessera_input imm32
        uiautomationcore dwmapi shcore)
elseif (TESSERA_X11_FOUND OR TESSERA_WAYLAND_FOUND)
    # Linux: the backend selector (window_linux.cpp) owns create_window() and Window::wait_for_events
    # and picks X11 or Wayland per session; each backend is compiled in only when its dev packages
    # were found, so an X11-only or Wayland-only box still builds (issue #155).
    target_sources(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/linux/window_linux.cpp)
    target_include_directories(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/linux)
    # OS appearance preferences (issue #187): the freedesktop settings portal watcher, one D-Bus
    # connection and one worker thread off the frame path, with the GTK settings.ini fallback. The
    # portal is SESSION-wide, not an X11 facility: both Linux backends read it, so this TU is
    # compiled whenever either backend is present (a Wayland-only box would otherwise have no
    # appearance source at all, and `window_wayland.cpp` would not link).
    target_sources(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/linux/appearance_portal.cpp)
    if (TESSERA_X11_FOUND)
        target_compile_definitions(tessera_hal_window PRIVATE TESSERA_HAS_X11=1)
        target_sources(tessera_hal_window PRIVATE
            ${PROJECT_SOURCE_DIR}/src/platform/linux/window.cpp
            # Full-screen (issue #232, window-chrome-v1.md section 13): EWMH _NET_WM_STATE, outside
            # window.cpp's size budget.
            ${PROJECT_SOURCE_DIR}/src/platform/linux/window_fullscreen_x11.cpp
            # Pointer shapes (issue #191): the font-cursor cache and XDefineCursor, same reason.
            ${PROJECT_SOURCE_DIR}/src/platform/linux/window_cursor_x11.cpp
            # Display capabilities (ADR-023, docs/specs/os-events-v1.md section 5): `_NET_WORKAREA`,
            # DPI and the RandR event normalization, outside window.cpp's size budget.
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_display.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_backdrop.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_input.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_input_translation.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2_translation.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_ime_composition.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2_events.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2_ime.cpp)
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
                ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_randr.cpp
                # RandR monitor/mode queries for the display-capability slice (ADR-023), a separate TU
                # so x11_display.cpp stays inside its file-size budget.
                ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_randr_monitors.cpp)
            target_compile_definitions(tessera_hal_window PRIVATE TESSERA_X11_RANDR=1)
            target_link_libraries(tessera_hal_window PRIVATE ${TESSERA_XRANDR_LIBRARY})
            message(STATUS "Xrandr found: Linux display-refresh query enabled")
        else()
            message(STATUS "Xrandr not found: Linux reports the unknown display refresh")
        endif()
    endif()
    if (TESSERA_WAYLAND_FOUND)
        # xdg-shell client code, generated from wayland-protocols' XML by wayland-scanner (a build
        # artifact: never committed, and exempt from the SPDX/format rules like other tool output).
        tessera_add_wayland_client_protocol(
            "${TESSERA_XDG_SHELL_XML}" xdg-shell
            TESSERA_XDG_SHELL_HEADER TESSERA_XDG_SHELL_SOURCE)
        target_sources(tessera_hal_window PRIVATE
            ${PROJECT_SOURCE_DIR}/src/platform/linux/window_wayland.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/window_wayland_dispatch.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/window_wayland_shell.cpp
            ${PROJECT_SOURCE_DIR}/src/platform/linux/window_wayland_output.cpp
            ${TESSERA_XDG_SHELL_SOURCE})
        target_include_directories(tessera_hal_window PRIVATE
            ${TESSERA_WAYLAND_GENERATED_DIR}
            ${TESSERA_WAYLAND_CLIENT_INCLUDE_DIRS})
        target_compile_definitions(tessera_hal_window PRIVATE TESSERA_HAS_WAYLAND=1)
        target_link_libraries(tessera_hal_window PRIVATE ${TESSERA_WAYLAND_CLIENT_LIBRARIES})
        target_link_directories(tessera_hal_window PRIVATE ${TESSERA_WAYLAND_CLIENT_LIBRARY_DIRS})
        message(STATUS "Wayland window backend enabled: Linux Wayland windows and xdg-shell")
    endif()
elseif (ANDROID)
    # Android has no window manager a native process can create a window in: the OS owns the
    # surface and hands the app an ANativeWindow*. The backend therefore adopts one instead of
    # creating it (issue #189), and `create_window()` keeps the documented "no backend" answer
    # because there is no descriptor that could produce a surface on its own.
    target_sources(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/android/window_android.cpp
        # Input translation (issue #189, mobile-integration.md section 3.2). The pure rules are in
        # android_input_translation.cpp, which includes no NDK header and is unit-tested on every
        # lane; android_input.cpp is the NDK-facing half.
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_input.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_input_translation.cpp
        # Window-metrics decode (issue #189, mobile-integration.md sections 3.5/3.6). The pure rules
        # are in android_metrics_translation.cpp, which includes no Android header and is
        # unit-tested on every lane.
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_metrics_translation.cpp
        # Java MotionEvent translation (issue #189, mobile-integration.md section 3.2). The shell is a
        # Java activity, so touch arrives as a Java object; the pure rules are in
        # android_motion_translation.cpp, which includes neither an Android header nor <jni.h> and is
        # unit-tested on every lane.
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_motion_translation.cpp
        # Point-to-caret translation (issue #189, android-lane.md section 3.13). A tap on a
        # custom-rendered field has to resolve to a caret, and the box arithmetic (physical pixels
        # to the content box, the padding, the density scale) is where a caret lands one character
        # off. The rules are in android_caret_translation.cpp, which includes no Android header and
        # is unit-tested on every lane.
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_caret_translation.cpp
        # Long-press and drag-to-select (issue #189, android-lane.md section 3.14). The platform
        # reports raw DOWN/MOVE/UP samples; what they MEAN is a small state machine, and it is in
        # android_selection_gesture.cpp, which includes no Android header and is unit-tested.
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_selection_gesture.cpp
        # IME translation (issue #189, mobile-integration.md section 3.3). The InputConnection
        # callbacks arrive as Java strings; the pure rules convert UTF-16 and offsets in
        # android_ime_translation.cpp, which includes no Android header and is unit-tested.
        # Accessibility mapping (issue #189, mobile-integration.md section 3.4): the role-to-widget-
        # class table and the platform's action ids. No Android header either, so the same table is
        # unit-tested on every lane and cannot drift from the values `javap -constants` reports.
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_ime_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/android/android_a11y_mapping.cpp)
    target_include_directories(tessera_hal_window PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/android)
    # libandroid provides ANativeWindow_acquire/release, the extent queries and AInputEvent.
    target_link_libraries(tessera_hal_window PUBLIC android)
else()
    target_sources(tessera_hal_window PRIVATE
        window_stub.cpp)
endif()

target_include_directories(tessera_hal_window PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# The base Window fallback and the public window_wait.hpp helper use the HAL steady clock.
target_link_libraries(tessera_hal_window PUBLIC tessera_hal_metrics tessera_hal_timer)

# The Linux appearance watcher speaks D-Bus, so the HAL links the bottom-level `tessera_dbus` client
# (the wire codec/SASL/transport, shared with the AT-SPI2 accessibility bridge). PRIVATE because the
# type is an implementation detail: no public HAL header exposes a D-Bus type, which is what keeps
# ADR-020's "the UI sees only normalized types" true. Windows needs no D-Bus at all.
if (TESSERA_X11_FOUND OR TESSERA_WAYLAND_FOUND)
    target_link_libraries(tessera_hal_window PRIVATE tessera_dbus)
endif()

target_compile_features(tessera_hal_window PUBLIC cxx_std_23)
