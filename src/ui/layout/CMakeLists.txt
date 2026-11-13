# SPDX-License-Identifier: MIT

add_library(tessera_layout STATIC
    apply_layout.cpp
    container_query_tracker.cpp
    damage_pass.cpp
    damage_tracker.cpp
    grid_layout.cpp
    grid_placement.cpp
    grid_tracks.cpp
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

# CSS Grid subset (issue #249): the deterministic corpus with expected boxes, the layout-signature
# coverage for the grid longhands and the 1,000-item timing. Registered here, not in the shared
# tests/unit registry, so a lane adding tests never edits a file another lane is working in.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(grid_layout_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/grid_layout_tests.cpp)
    target_link_libraries(grid_layout_tests PRIVATE tessera_layout)
    add_test(NAME grid_layout COMMAND grid_layout_tests)

    add_executable(grid_signature_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/grid_signature_tests.cpp)
    target_link_libraries(grid_signature_tests PRIVATE tessera_layout)
    add_test(NAME grid_signature COMMAND grid_signature_tests)
endif()
