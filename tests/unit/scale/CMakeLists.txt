# SPDX-License-Identifier: MIT

# Render-scale/DPI quantization tests; the slice registers its own executables and ctest names
# here so no two slices edit `tests/unit/CMakeLists.txt` at the same time.

add_executable(scale_quantize_tests
    scale_quantize_tests.cpp)

target_link_libraries(scale_quantize_tests PRIVATE tessera_scale)

add_test(NAME scale_quantize COMMAND scale_quantize_tests)

add_executable(scale_state_tests
    scale_state_tests.cpp)

target_link_libraries(scale_state_tests PRIVATE tessera_scale)

add_test(NAME scale_state COMMAND scale_state_tests)

# The per-scale atlas set is exercised against the committed fixture font; TESSERA_FONT_FIXTURE is
# defined by the parent tests/unit directory and resolved to an absolute path there.
add_executable(scale_atlas_tests
    scale_atlas_tests.cpp)

target_link_libraries(scale_atlas_tests PRIVATE tessera_text)

add_test(NAME scale_atlas COMMAND scale_atlas_tests "${TESSERA_FONT_FIXTURE}")
