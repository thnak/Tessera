# SPDX-License-Identifier: MIT

# Accessibility semantics layer (docs/specs/accessibility-v1.md): turns the scene graph plus node
# metadata, computed style, focus, and scroll state into a bounded, deterministic flat snapshot,
# then serves it over an in-tree AT-SPI2 D-Bus client. No platform API is called here; bridges
# consume the snapshot later.
#
# The D-Bus client itself (address parsing, SASL, the wire codec, the transport seam) is the
# bottom-level `tessera_dbus` target in src/dbus: the HAL's Linux appearance watcher observes
# `org.freedesktop.portal.Settings` through the same client, and the HAL may not depend on this
# UI-layer library. This target links it and uses its types directly.

find_package(Threads REQUIRED)

set(TESSERA_A11Y_SOURCES
    a11y_roles.cpp
    a11y_tree.cpp
    announcement_queue.cpp
    live_regions.cpp
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
    uia_bridge_jobs.cpp
    # The pull-side provider Android's `AccessibilityNodeProvider` queries (issue #189). Android is
    # the only platform whose accessibility API is synchronous/pull rather than push, so this is a
    # separate type from the UIA and AT-SPI bridges rather than another branch in one of them.
    snapshot_provider.cpp)

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
    tessera_dbus
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

# The announcement queue behind `useAnnounce` (ADR-064 item 7, issue #629): the bounded,
# de-duplicating list the accessibility bridge drains. Registered here rather than in the shared
# tests/unit registry, so a lane adding tests never edits a file another lane is working in.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(announcement_queue_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/announcement_queue_tests.cpp)
    target_link_libraries(announcement_queue_tests PRIVATE tessera_accessibility)
    add_test(NAME announcement_queue COMMAND announcement_queue_tests)

    # The pull-side provider Android queries (issue #189): slot stability across a rebuild, stale
    # ids, the value-not-label rule, and the bounded action queue.
    add_executable(snapshot_provider_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/a11y/snapshot_provider_tests.cpp)
    target_link_libraries(snapshot_provider_tests PRIVATE tessera_accessibility)
    add_test(NAME snapshot_provider COMMAND snapshot_provider_tests)
endif ()



