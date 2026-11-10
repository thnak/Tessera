# SPDX-License-Identifier: MIT

# Accessibility semantics layer (docs/specs/accessibility-v1.md): turns the scene graph plus node
# metadata, computed style, focus, and scroll state into a bounded, deterministic flat snapshot,
# then serves it over an in-tree AT-SPI2 D-Bus client. No platform API is called here; bridges
# consume the snapshot later.

find_package(Threads REQUIRED)

add_library(tessera_accessibility STATIC
    a11y_roles.cpp
    a11y_tree.cpp
    dbus_address.cpp
    dbus_transport.cpp
    dbus_unix_transport.cpp
    dbus_wire.cpp
    dbus_reader.cpp
    dbus_message.cpp
    dbus_auth.cpp
    atspi_mapping.cpp
    atspi_events.cpp
    atspi_bridge.cpp
    atspi_methods.cpp
    atspi_properties.cpp
    atspi_session.cpp
    atspi_session_handshake.cpp
    atspi_session_pump.cpp
    atspi_session_status.cpp)

target_include_directories(tessera_accessibility PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_accessibility PUBLIC
    tessera_scene
    tessera_style
    tessera_schema
    Threads::Threads)

target_compile_features(tessera_accessibility PUBLIC cxx_std_23)
