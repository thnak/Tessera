# SPDX-License-Identifier: MIT

# The `ui/services.json` data and reader (docs/specs/services-manifest-v1.md, issue #177). Two targets
# so the analysis layer can take the plain data without linking the JSON decoder:
#   tessera_services_types     header-only structs, no dependency (the Wasm designer links this)
#   tessera_services_manifest  the strict reader over `tessera_harness`, and the typings generator
add_library(tessera_services_types INTERFACE)
target_include_directories(tessera_services_types
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(tessera_services_types INTERFACE cxx_std_23)

add_library(tessera_services_manifest STATIC
    src/manifest.cpp
    src/typings.cpp)

target_link_libraries(tessera_services_manifest PUBLIC tessera_services_types tessera_harness)
