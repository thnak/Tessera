# SPDX-License-Identifier: MIT

# The schema dump tool (docs/specs/jsx-intrinsics-v1/machine-readable.md section 4). The core is a
# small library so the unit golden test and the CLI write the same bytes.
add_library(tessera_schema_dump_core STATIC
    src/dump_dts.cpp
    src/dump_json.cpp)

target_include_directories(tessera_schema_dump_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_schema_dump_core
    PUBLIC tessera_schema
    PRIVATE tessera_ir)

add_executable(tessera_schema_dump
    src/main.cpp)

target_link_libraries(tessera_schema_dump PRIVATE tessera_schema_dump_core)
