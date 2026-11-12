# SPDX-License-Identifier: MIT

# Shader-manifest validation tests; the slice registers its own executables and ctest names here
# so no two slices edit `tests/unit/CMakeLists.txt` at the same time.

add_executable(manifest_parse_tests
    manifest_parse_tests.cpp)

target_link_libraries(manifest_parse_tests PRIVATE tessera_schema)

add_test(NAME manifest_parse COMMAND manifest_parse_tests)

add_executable(manifest_limits_tests
    manifest_limits_tests.cpp)

target_link_libraries(manifest_limits_tests PRIVATE tessera_schema)

add_test(NAME manifest_limits COMMAND manifest_limits_tests)

add_executable(manifest_keys_tests
    manifest_keys_tests.cpp)

target_link_libraries(manifest_keys_tests PRIVATE tessera_schema)

add_test(NAME manifest_keys COMMAND manifest_keys_tests)

add_executable(manifest_stages_tests
    manifest_stages_tests.cpp)

target_link_libraries(manifest_stages_tests PRIVATE tessera_schema)

add_test(NAME manifest_stages COMMAND manifest_stages_tests)

add_executable(manifest_errors_tests
    manifest_errors_tests.cpp)

target_link_libraries(manifest_errors_tests PRIVATE tessera_schema)

add_test(NAME manifest_errors COMMAND manifest_errors_tests)

# The build-step contract: the repository manifest loads, a broken copy fails with the typed
# error, and the batched2d entry matches Vertex2D and the authored GLSL (section 5/9).
add_executable(manifest_build_step_tests
    manifest_build_step_tests.cpp)

target_link_libraries(manifest_build_step_tests PRIVATE tessera_schema)

target_include_directories(manifest_build_step_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

target_compile_definitions(manifest_build_step_tests PRIVATE
    TESSERA_SHADER_MANIFEST_PATH="${PROJECT_SOURCE_DIR}/tools/shaders/manifest.json"
    TESSERA_SHADER_GLSL_DIR="${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/shaders")

add_test(NAME manifest_build_step COMMAND manifest_build_step_tests)
