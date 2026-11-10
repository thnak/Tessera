# SPDX-License-Identifier: MIT

# Accessibility semantics layer (docs/specs/accessibility-v1.md): turns the scene graph plus node
# metadata, computed style, focus, and scroll state into a bounded, deterministic flat snapshot,
# then serves it over an in-tree AT-SPI2 D-Bus client. No platform API is called here; bridges
# consume the snapshot later.

find_package(Threads REQUIRED)

set(TESSERA_A11Y_SOURCES
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
    atspi_session_status.cpp
    uia_mapping.cpp
    uia_error.cpp
    uia_handoff.cpp
    uia_events.cpp
    uia_provider.cpp
    uia_host.cpp
    uia_bridge.cpp
    uia_bridge_jobs.cpp)

# The Windows UI Automation bridge is the only code in this layer that touches a platform SDK. It
# is compiled on Windows only; every other platform builds the same public headers plus a stub
# apartment/root seam, so a Linux/macOS/Android/iOS configure never includes a Windows header.
if (WIN32)
    list(APPEND TESSERA_A11Y_SOURCES
        uia_apartment_win.cpp
        uia_platform_win.cpp
        uia_com.cpp
        uia_com_fragment.cpp
        uia_com_patterns.cpp
        uia_sdk_probe.cpp)
else()
    list(APPEND TESSERA_A11Y_SOURCES
        uia_apartment_stub.cpp
        uia_platform_stub.cpp)
endif()

add_library(tessera_accessibility STATIC ${TESSERA_A11Y_SOURCES})

target_include_directories(tessera_accessibility PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_accessibility PUBLIC
    tessera_scene
    tessera_style
    tessera_schema
    Threads::Threads)

# Static libraries do not link, but CMake carries PRIVATE link dependencies to the executables that
# consume them, so a Windows app that uses the UIA provider links the system automation/COM
# libraries without repeating them (system components stay dynamic, ADR-012).
if (WIN32)
    target_link_libraries(tessera_accessibility PRIVATE uiautomationcore ole32)
endif()

target_compile_features(tessera_accessibility PUBLIC cxx_std_23)



