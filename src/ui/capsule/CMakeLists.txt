# SPDX-License-Identifier: MIT

# LiveCapsule (docs/specs/live-capsule-v1.md, issue #224): placement, states, morph and the
# activity host. Pure logic over plain rectangles and the spring runtime; no GPU and no window.
add_library(tessera_capsule STATIC
    capsule_morph.cpp
    capsule_placement.cpp
    capsule_state.cpp
    live_capsule.cpp
    live_capsule_host.cpp
    live_capsule_poster.cpp
    slot_measure.cpp)

target_include_directories(tessera_capsule PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_capsule PUBLIC tessera_animation tessera_scene tessera_accessibility
    tessera_runtime tessera_text)

target_compile_features(tessera_capsule PUBLIC cxx_std_23)

# Registered here, not in the shared tests/unit registry, so a lane adding tests never edits a file
# another lane is working in.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(live_capsule_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/live_capsule_tests.cpp)
    target_link_libraries(live_capsule_tests PRIVATE tessera_capsule)
    add_test(NAME live_capsule COMMAND live_capsule_tests)

    # Cross-thread start/update/end through the completion queue (issue #968).
    add_executable(live_capsule_poster_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/live_capsule_poster_tests.cpp)
    target_link_libraries(live_capsule_poster_tests PRIVATE tessera_capsule)
    add_test(NAME live_capsule_poster COMMAND live_capsule_poster_tests)

    # The component in a real AppHost: semantics, focus, parts, in-place update, idle.
    add_executable(live_capsule_component_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/live_capsule_component_tests.cpp)
    target_include_directories(live_capsule_component_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(live_capsule_component_tests PRIVATE tessera_capsule tessera_app
        tessera_runtime tessera_accessibility)
    add_test(NAME live_capsule_component COMMAND live_capsule_component_tests)
endif()
