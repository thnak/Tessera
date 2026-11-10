# SPDX-License-Identifier: MIT

# Tests for `tessera_boundary`; the slice registers its own executables and ctest names here so
# parallel slices never edit `tests/unit/CMakeLists.txt` at the same time. Every ctest name starts
# with `boundary` so the lane can run `ctest -R boundary`.

add_executable(boundary_model_tests
    boundary_model_tests.cpp)

target_link_libraries(boundary_model_tests PRIVATE tessera_boundary)

add_test(NAME boundary_model COMMAND boundary_model_tests)

add_executable(boundary_retry_tests
    boundary_retry_tests.cpp)

target_link_libraries(boundary_retry_tests PRIVATE tessera_boundary)

add_test(NAME boundary_retry COMMAND boundary_retry_tests)

add_executable(boundary_nested_tests
    boundary_nested_tests.cpp)

target_link_libraries(boundary_nested_tests PRIVATE tessera_boundary)

add_test(NAME boundary_nested COMMAND boundary_nested_tests)

add_executable(boundary_guard_tests
    boundary_guard_tests.cpp)

target_link_libraries(boundary_guard_tests PRIVATE tessera_boundary)

add_test(NAME boundary_guard COMMAND boundary_guard_tests)
