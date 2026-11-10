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
endif()
