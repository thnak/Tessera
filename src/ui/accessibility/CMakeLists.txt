# SPDX-License-Identifier: MIT

# Accessibility semantics layer (docs/specs/accessibility-v1.md): turns the scene graph plus node
# metadata, computed style, focus, and scroll state into a bounded, deterministic flat snapshot.
# No platform API is called here; bridges consume the snapshot later.

add_library(tessera_accessibility STATIC
    a11y_tree.cpp)

target_include_directories(tessera_accessibility PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_accessibility PUBLIC
    tessera_scene
    tessera_style
    tessera_schema)

target_compile_features(tessera_accessibility PUBLIC cxx_std_23)
