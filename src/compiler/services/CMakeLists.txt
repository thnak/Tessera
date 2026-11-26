# SPDX-License-Identifier: MIT

# The `ui/services.json` reader (docs/specs/services-manifest-v1.md, issue #177): the service
# interfaces TSX may name and the methods it may call. A leaf library over the strict JSON decoder, so
# the front end, codegen, the generated typings and the Studio mock services share one reader.
add_library(tessera_services_manifest STATIC
    src/manifest.cpp)

target_include_directories(tessera_services_manifest
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(tessera_services_manifest PUBLIC cxx_std_23)

target_link_libraries(tessera_services_manifest PUBLIC tessera_harness)
