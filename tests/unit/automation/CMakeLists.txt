# SPDX-License-Identifier: MIT

# Tests for `tessera_automation`; the slice registers its own executables and ctest names here so
# parallel slices never edit `tests/unit/CMakeLists.txt` at the same time.

add_executable(automation_sha256_tests
    sha256_tests.cpp)

target_link_libraries(automation_sha256_tests PRIVATE tessera_automation)

add_test(NAME automation_sha256 COMMAND automation_sha256_tests)

add_executable(automation_hash_id_tests
    id_tests.cpp)

target_link_libraries(automation_hash_id_tests PRIVATE tessera_automation)

add_test(NAME automation_hash_id COMMAND automation_hash_id_tests)

add_executable(automation_snapshot_tests
    snapshot_tests.cpp)

target_link_libraries(automation_snapshot_tests PRIVATE tessera_automation)

add_test(NAME automation_snapshot COMMAND automation_snapshot_tests)

add_executable(automation_snapshot_json_tests
    snapshot_json_tests.cpp)

target_link_libraries(automation_snapshot_json_tests PRIVATE tessera_automation)

add_test(NAME automation_snapshot_json COMMAND automation_snapshot_json_tests)
