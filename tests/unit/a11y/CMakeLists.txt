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
