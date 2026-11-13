# SPDX-License-Identifier: MIT

# Headless generated-UI integration demo (task-56). Canonical target definition: it is executed
# either by a future `add_subdirectory(tests/integration)` (root CMakeLists, Lead-owned) or by the
# fallback `include()` in tests/unit/CMakeLists.txt. The guards keep both paths idempotent.

if (TARGET generated_ui_tests)
    return()
endif()
if (NOT TARGET tessera_compiler_demo)
    return()
endif()

set(TESSERA_DEMO_FONT_FIXTURE
    "${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
get_filename_component(TESSERA_DEMO_FONT_FIXTURE "${TESSERA_DEMO_FONT_FIXTURE}" REALPATH)

add_executable(generated_ui_tests
    ${CMAKE_CURRENT_LIST_DIR}/generated_ui_demo.hpp
    ${CMAKE_CURRENT_LIST_DIR}/generated_ui_tests.cpp)

target_link_libraries(generated_ui_tests PRIVATE
    tessera_compiler_demo
    tessera_scene
    tessera_style
    tessera_layout
    tessera_paint
    tessera_text
    tessera_text_edit
    tessera_software
    tessera_golden
    tessera_stb)

# Generated demo headers live in the codegen build tree; the CPU paint sink is local to the demo
# (generated_ui_demo.hpp) because the PoC sink is still owned/changing with the assets lane.
target_compile_features(generated_ui_tests PRIVATE cxx_std_23)

add_test(
    NAME generated_ui
    COMMAND generated_ui_tests
        ${TESSERA_DEMO_FONT_FIXTURE}
        ${PROJECT_SOURCE_DIR}/tests/golden/baseline/generated_ui)

# Memory benchmark on the same headless sample (issue #13): resident growth of the build, the first
# paint and a scripted click loop, compared with tests/golden/memory/generated_ui.txt.
add_executable(generated_ui_memory_tests
    ${CMAKE_CURRENT_LIST_DIR}/generated_ui_demo.hpp
    ${CMAKE_CURRENT_LIST_DIR}/generated_ui_memory_tests.cpp)

target_link_libraries(generated_ui_memory_tests PRIVATE
    tessera_compiler_demo
    tessera_diagnostics
    tessera_scene
    tessera_style
    tessera_layout
    tessera_paint
    tessera_text
    tessera_text_edit
    tessera_software
    tessera_golden
    tessera_stb)

target_compile_features(generated_ui_memory_tests PRIVATE cxx_std_23)

add_test(
    NAME generated_ui_memory
    COMMAND generated_ui_memory_tests
        ${TESSERA_DEMO_FONT_FIXTURE}
        ${PROJECT_SOURCE_DIR}/tests/golden/memory/generated_ui.txt)
