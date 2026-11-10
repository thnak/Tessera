# SPDX-License-Identifier: MIT

# Virtualized list geometry and item state (`docs/specs/virtualization-v1.md`). The slice owns its
# targets here so parallel slices never edit the root subdirectory list at the same time.

add_library(tessera_virtual STATIC
    collection_semantics.cpp
    grid_rows.cpp
    item_geometry.cpp
    key_index.cpp
    node_recycler.cpp
    scroll_anchor.cpp
    item_state_store.cpp
    virtual_range.cpp
    virtual_range_focus.cpp
    virtual_range_query.cpp)

target_include_directories(tessera_virtual PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# The recycler builds and clears scene nodes (`ui::Node`); the geometry/state primitives stay
# header-only independent of the scene graph.
target_link_libraries(tessera_virtual PUBLIC tessera_scene)

target_compile_features(tessera_virtual PUBLIC cxx_std_23)
