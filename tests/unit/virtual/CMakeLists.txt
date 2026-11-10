# SPDX-License-Identifier: MIT

# Tests for `tessera_virtual`; the slice registers its own executables and ctest names here so
# parallel slices never edit `tests/unit/CMakeLists.txt` at the same time.

add_executable(virtual_geometry_tests
    virtual_geometry_tests.cpp)

target_link_libraries(virtual_geometry_tests PRIVATE tessera_virtual)

add_test(NAME virtual_geometry COMMAND virtual_geometry_tests)

add_executable(virtual_geometry_scan_tests
    virtual_geometry_scan_tests.cpp)

target_link_libraries(virtual_geometry_scan_tests PRIVATE tessera_virtual)

add_test(NAME virtual_geometry_scan COMMAND virtual_geometry_scan_tests)

add_executable(virtual_allocation_tests
    virtual_allocation_tests.cpp)

target_link_libraries(virtual_allocation_tests PRIVATE tessera_virtual)

add_test(NAME virtual_allocations COMMAND virtual_allocation_tests)

add_executable(virtual_anchor_tests
    virtual_anchor_tests.cpp)

target_link_libraries(virtual_anchor_tests PRIVATE tessera_virtual)

add_test(NAME virtual_anchor COMMAND virtual_anchor_tests)

add_executable(virtual_state_tests
    virtual_state_tests.cpp)

target_link_libraries(virtual_state_tests PRIVATE tessera_virtual)

add_test(NAME virtual_state COMMAND virtual_state_tests)

add_executable(virtual_data_source_tests
    virtual_data_source_tests.cpp)

target_link_libraries(virtual_data_source_tests PRIVATE tessera_virtual tessera_scene)

add_test(NAME virtual_data_source COMMAND virtual_data_source_tests)

add_executable(virtual_key_index_tests
    virtual_key_index_tests.cpp)

target_link_libraries(virtual_key_index_tests PRIVATE tessera_virtual)

add_test(NAME virtual_key_index COMMAND virtual_key_index_tests)

add_executable(virtual_pool_tests
    virtual_pool_tests.cpp)

target_link_libraries(virtual_pool_tests PRIVATE tessera_virtual)

add_test(NAME virtual_pool COMMAND virtual_pool_tests)
