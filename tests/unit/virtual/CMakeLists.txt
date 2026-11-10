# SPDX-License-Identifier: MIT

# Tests for `tessera_virtual`; the slice registers its own executables and ctest names here so
# parallel slices never edit `tests/unit/CMakeLists.txt` at the same time.

add_executable(virtual_geometry_tests
    virtual_geometry_tests.cpp)

target_link_libraries(virtual_geometry_tests PRIVATE tessera_virtual)

add_test(NAME virtual_geometry COMMAND virtual_geometry_tests)

add_executable(virtual_anchor_tests
    virtual_anchor_tests.cpp)

target_link_libraries(virtual_anchor_tests PRIVATE tessera_virtual)

add_test(NAME virtual_anchor COMMAND virtual_anchor_tests)

add_executable(virtual_state_tests
    virtual_state_tests.cpp)

target_link_libraries(virtual_state_tests PRIVATE tessera_virtual)

add_test(NAME virtual_state COMMAND virtual_state_tests)
