# SPDX-License-Identifier: MIT

# Native Metal backend (ADR-053). No MoltenVK: this is the Metal API directly, in Objective-C++
# confined to this directory.
#
# Two halves, the same split the WebGPU backend uses:
#   - the API-free mapping tables and the MSL shader registry, built everywhere (cheap, pure) so
#     their unit tests run in the ordinary host ctest on Windows and Linux;
#   - the device half, which needs <Metal/Metal.h> and therefore only builds on Apple platforms.
#
# Tier 2 in-tree (ADR-049 section 4, docs/guides/writing-a-backend.md section 2) until the backend
# conformance suite passes on a macOS lane. The option is OFF by default and in no preset.

option(TESSERA_BUILD_METAL "Build the native Metal backend (Apple platforms only)" OFF)

add_library(tessera_metal_mapping STATIC
    src/metal_mapping.cpp)

target_include_directories(tessera_metal_mapping
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(tessera_metal_mapping PUBLIC tessera_rhi)
target_compile_features(tessera_metal_mapping PUBLIC cxx_std_23)

if (TESSERA_BUILD_METAL)
    if (NOT APPLE)
        message(FATAL_ERROR
            "TESSERA_BUILD_METAL=ON needs an Apple platform: the backend is Objective-C++ "
            "against <Metal/Metal.h> (ADR-053).")
    endif()

    enable_language(OBJCXX)

    # The device half. Objective-C++ (ARC) is confined to this directory: no `id<...>` or `NS*`
    # type may appear in a header outside it (docs/design/gpu/metal-backend.md section 4).
    add_library(tessera_rhi_metal STATIC
        src/metal_device.mm)

    target_include_directories(tessera_rhi_metal
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(tessera_rhi_metal PUBLIC tessera_metal_mapping tessera_rhi_shared)
    target_compile_features(tessera_rhi_metal PUBLIC cxx_std_23)

    # ARC for the Objective-C++ translation units only; the C++ headers stay plain C++.
    set_source_files_properties(src/metal_device.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_compile_options(tessera_rhi_metal PRIVATE "-fobjc-arc")

    # Metal, Foundation and QuartzCore (CAMetalLayer) are system frameworks: dynamic, as ADR-012
    # requires for OS components.
    target_link_libraries(tessera_rhi_metal PRIVATE
        "-framework Metal"
        "-framework Foundation"
        "-framework QuartzCore")
endif()
