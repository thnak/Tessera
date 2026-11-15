# SPDX-License-Identifier: MIT

# The in-tree D-Bus client (docs/specs/accessibility-v1.md section 7, docs/design/hal.md section
# 1.7). The repo speaks the wire protocol itself instead of linking libdbus/glib (ADR-012, static
# linking), so this library is the whole protocol surface: address parsing, SASL EXTERNAL
# authentication, the bounded wire codec, and the transport seam.
#
# It sits BELOW both consumers on purpose. The AT-SPI2 accessibility bridge is one consumer
# (`tessera_accessibility`), and the Linux appearance watcher in the HAL is the other: the
# freedesktop `org.freedesktop.portal.Settings` color-scheme/enable-animations source is observed
# through the same client, and the HAL may not depend on the UI layer that owns the bridge. Hence a
# bottom-level target here rather than headers under `include/tessera/ui/accessibility/`.
#
# The codec half is platform-neutral and unit-tested on every lane. The transport half has a
# deterministic in-process loopback pair (no daemon, no socket, no clock) plus the POSIX
# UNIX-socket transport, which compiles its "unavailable" arm everywhere else.

add_library(tessera_dbus STATIC
    address.cpp
    auth.cpp
    message.cpp
    reader.cpp
    transport.cpp
    unix_transport.cpp
    wire.cpp)

target_include_directories(tessera_dbus PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_dbus PUBLIC cxx_std_23)
