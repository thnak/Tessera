# SPDX-License-Identifier: MIT

add_library(tessera_layout STATIC
    apply_layout.cpp
    container_query_tracker.cpp
    damage_pass.cpp
    damage_tracker.cpp
    layout_engine.cpp
    layout_observer.cpp
    layout_observer_settle.cpp
    layout_read_model.cpp
    layout_style.cpp
    layout_scheduler.cpp
    layout_signature.cpp
    pixel_snap.cpp)

target_include_directories(tessera_layout PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_layout PUBLIC
    tessera_damage
    tessera_diagnostics
    tessera_scene
    tessera_style
    tessera_text
    tessera_yoga)

target_compile_features(tessera_layout PUBLIC cxx_std_23)
