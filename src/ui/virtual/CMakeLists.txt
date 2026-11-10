# SPDX-License-Identifier: MIT

# Virtualized list geometry and item state (`docs/specs/virtualization-v1.md`). The slice owns its
# targets here so parallel slices never edit the root subdirectory list at the same time.

add_library(tessera_virtual STATIC
    item_geometry.cpp
    scroll_anchor.cpp
    item_state_store.cpp)

target_include_directories(tessera_virtual PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_virtual PUBLIC cxx_std_23)
