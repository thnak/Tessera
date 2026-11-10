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
