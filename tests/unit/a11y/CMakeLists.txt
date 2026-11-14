# SPDX-License-Identifier: MIT

# AT-SPI2 D-Bus wire client tests; the slice registers its own executables and ctest names here so
# no two slices edit `tests/unit/CMakeLists.txt` at the same time. Every ctest name starts with
# `a11y` so the lane can run `ctest -R a11y`, and no test needs a real bus daemon or a socket.

add_executable(a11y_dbus_address_tests
    a11y_dbus_address_tests.cpp)

target_link_libraries(a11y_dbus_address_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_dbus_address COMMAND a11y_dbus_address_tests)

add_executable(a11y_dbus_transport_tests
    a11y_dbus_transport_tests.cpp)

target_link_libraries(a11y_dbus_transport_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_dbus_transport COMMAND a11y_dbus_transport_tests)

add_executable(a11y_dbus_wire_tests
    a11y_dbus_wire_tests.cpp)

target_link_libraries(a11y_dbus_wire_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_dbus_wire COMMAND a11y_dbus_wire_tests)

add_executable(a11y_dbus_wire_bounds_tests
    a11y_dbus_wire_bounds_tests.cpp)

target_link_libraries(a11y_dbus_wire_bounds_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_dbus_wire_bounds COMMAND a11y_dbus_wire_bounds_tests)

add_executable(a11y_dbus_auth_tests
    a11y_dbus_auth_tests.cpp)

target_link_libraries(a11y_dbus_auth_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_dbus_auth COMMAND a11y_dbus_auth_tests)

add_executable(a11y_atspi_mapping_tests
    a11y_atspi_mapping_tests.cpp)

target_link_libraries(a11y_atspi_mapping_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_atspi_mapping COMMAND a11y_atspi_mapping_tests)

add_executable(a11y_atspi_events_tests
    a11y_atspi_events_tests.cpp)

target_link_libraries(a11y_atspi_events_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_atspi_events COMMAND a11y_atspi_events_tests)

add_executable(a11y_atspi_bridge_tests
    a11y_atspi_bridge_tests.cpp)

target_link_libraries(a11y_atspi_bridge_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_atspi_bridge COMMAND a11y_atspi_bridge_tests)

add_executable(a11y_atspi_bridge_events_tests
    a11y_atspi_bridge_events_tests.cpp)

target_link_libraries(a11y_atspi_bridge_events_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_atspi_bridge_events COMMAND a11y_atspi_bridge_events_tests)

add_executable(a11y_atspi_properties_tests
    a11y_atspi_properties_tests.cpp)

target_link_libraries(a11y_atspi_properties_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_atspi_properties COMMAND a11y_atspi_properties_tests)

add_executable(a11y_atspi_session_tests
    a11y_atspi_session_tests.cpp)

target_link_libraries(a11y_atspi_session_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_atspi_session COMMAND a11y_atspi_session_tests)

add_executable(a11y_atspi_session_pump_tests
    a11y_atspi_session_pump_tests.cpp)

target_link_libraries(a11y_atspi_session_pump_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_atspi_session_pump COMMAND a11y_atspi_session_pump_tests)

add_executable(a11y_uia_mapping_tests
    a11y_uia_mapping_tests.cpp)
target_link_libraries(a11y_uia_mapping_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_mapping COMMAND a11y_uia_mapping_tests)

add_executable(a11y_uia_handoff_tests
    a11y_uia_handoff_tests.cpp)

target_link_libraries(a11y_uia_handoff_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_handoff COMMAND a11y_uia_handoff_tests)

add_executable(a11y_uia_bridge_tests
    a11y_uia_bridge_tests.cpp)

target_link_libraries(a11y_uia_bridge_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_bridge COMMAND a11y_uia_bridge_tests)

add_executable(a11y_uia_bridge_lifecycle_tests
    a11y_uia_bridge_lifecycle_tests.cpp)

target_link_libraries(a11y_uia_bridge_lifecycle_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_bridge_lifecycle COMMAND a11y_uia_bridge_lifecycle_tests)

add_executable(a11y_uia_provider_tests
    a11y_uia_provider_tests.cpp)

target_link_libraries(a11y_uia_provider_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_provider COMMAND a11y_uia_provider_tests)

# Host wiring: the WM_GETOBJECT handshake runs against a fake COM apartment and a fake UIA platform
# seam, so it is deterministic on every platform and needs no desktop session.
add_executable(a11y_uia_host_getobject_tests
    a11y_uia_host_getobject_tests.cpp)

target_link_libraries(a11y_uia_host_getobject_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_host_getobject COMMAND a11y_uia_host_getobject_tests)

# Dirty-driven publication and the tick-boundary action drain.
add_executable(a11y_uia_host_tick_tests
    a11y_uia_host_tick_tests.cpp)

target_link_libraries(a11y_uia_host_tick_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_host_tick COMMAND a11y_uia_host_tick_tests)

# Coalescing/dedup queue and the snapshot diff that derives StructureChanged/PropertyChanged/focus
# events (platform independent).
add_executable(a11y_uia_events_tests
    a11y_uia_events_tests.cpp)

target_link_libraries(a11y_uia_events_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_events COMMAND a11y_uia_events_tests)

# Host-level event raising: the flush job runs on the bridge thread and a static tick raises
# nothing.
add_executable(a11y_uia_host_events_tests
    a11y_uia_host_events_tests.cpp)

target_link_libraries(a11y_uia_host_events_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_uia_host_events COMMAND a11y_uia_host_events_tests)

# The Windows UI Automation provider is the only platform-specific code in this directory. The
# probe test proves the SDK headers and the uiautomationcore/ole32 link work from the static
# accessibility library; the COM test drives the real provider vtable without a UIA client. Both are
# absent everywhere else, where the same public headers must still compile (that is what
# `a11y_uia_mapping` and `a11y_uia_provider` check on Linux/macOS).
if (WIN32)
    add_executable(a11y_uia_compile_check_tests
        a11y_uia_compile_check_tests.cpp)

    target_link_libraries(a11y_uia_compile_check_tests PRIVATE tessera_accessibility)
    target_include_directories(a11y_uia_compile_check_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src/ui/accessibility)

    add_test(NAME a11y_uia_compile_check COMMAND a11y_uia_compile_check_tests)

    # The real Win32 window: WM_GETOBJECT reaches the accessibility handshake and falls back to
    # DefWindowProc when nothing answers.
    add_executable(a11y_uia_window_getobject_tests
        a11y_uia_window_getobject_tests.cpp)

    target_link_libraries(a11y_uia_window_getobject_tests PRIVATE tessera_accessibility
        tessera_hal_window)

    add_test(NAME a11y_uia_window_getobject COMMAND a11y_uia_window_getobject_tests)

    add_executable(a11y_uia_com_tests
        a11y_uia_com_tests.cpp)

    target_link_libraries(a11y_uia_com_tests PRIVATE tessera_accessibility)
    target_include_directories(a11y_uia_com_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src/ui/accessibility)

    add_test(NAME a11y_uia_com COMMAND a11y_uia_com_tests)

    add_executable(a11y_uia_com_nav_tests
        a11y_uia_com_nav_tests.cpp)

    target_link_libraries(a11y_uia_com_nav_tests PRIVATE tessera_accessibility)
    target_include_directories(a11y_uia_com_nav_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src/ui/accessibility)

    add_test(NAME a11y_uia_com_nav COMMAND a11y_uia_com_nav_tests)

    # The root's HostRawElementProvider answer: a fake source pins the COM contract (AddRef'd
    # provider, E_FAIL for no/absent/failed handle, S_OK + null for a child).
    add_executable(a11y_uia_com_host_element_tests
        a11y_uia_com_host_element_tests.cpp)

    target_link_libraries(a11y_uia_com_host_element_tests PRIVATE tessera_accessibility)
    target_include_directories(a11y_uia_com_host_element_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/src/ui/accessibility)

    add_test(NAME a11y_uia_com_host_element COMMAND a11y_uia_com_host_element_tests)

    # The real window + real UIA platform: the host element comes from the window's HWND, is cached,
    # and a missing handle fails cleanly. The client call runs on a helper thread while the window
    # thread pumps, so the test never depends on a screen reader.
    add_executable(a11y_uia_window_host_element_tests
        a11y_uia_window_host_element_tests.cpp)

    target_link_libraries(a11y_uia_window_host_element_tests PRIVATE tessera_accessibility
        tessera_hal_window)

    add_test(NAME a11y_uia_window_host_element COMMAND a11y_uia_window_host_element_tests)
endif()

# Host-wiring integration (issues #185/#186, accessibility-v1.md section 12.3): the app's own
# `AccessibilityHost` controller drives the real UIA host (fake apartment + fake platform seams) and
# the real AT-SPI session (loopback transport + synthetic peer). Pure CPU: no window, no screen
# reader, no bus daemon, no socket. Links the app library's private controller header via the
# include directory below, plus the harness fake window.
add_executable(app_accessibility_host_tests
    app_accessibility_host_tests.cpp
    app_accessibility_host_atspi_tests.cpp)

target_link_libraries(app_accessibility_host_tests PRIVATE
    tessera_app
    tessera_harness_testing)

target_include_directories(app_accessibility_host_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/src/app)

add_test(NAME app_accessibility_host COMMAND app_accessibility_host_tests)
