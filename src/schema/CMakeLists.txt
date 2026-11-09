# SPDX-License-Identifier: MIT

# Shared machine-readable intrinsic/style schema (docs/specs/jsx-intrinsics-v1/machine-readable.md
# Option C, compiler slice 12 of docs/design/jsx-ir-lowering.md section 9). It is a leaf library:
# the compiler, the designer and the runtime all link it and it links nothing from the project.
add_library(tessera_schema STATIC
    src/aliases.cpp
    src/common.cpp
    src/elements.cpp
    src/lookups.cpp
    src/style.cpp
    src/types.cpp)

target_include_directories(tessera_schema
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(tessera_schema PUBLIC cxx_std_23)

# Layering rule of machine-readable.md section 4: the schema must not link a project library, so
# the compiler can link it without linking the UI or the IR.
get_target_property(_tessera_schema_links tessera_schema LINK_LIBRARIES)
if (_tessera_schema_links)
    message(FATAL_ERROR
        "tessera_schema must not link any library, found: ${_tessera_schema_links}")
endif ()
