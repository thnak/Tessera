# SPDX-License-Identifier: MIT

# Headless harness protocol tests; the slice registers its own executables and ctest names here so
# no two slices edit `tests/unit/CMakeLists.txt` at the same time.

add_executable(harness_framing_tests
    framing_tests.cpp)

target_link_libraries(harness_framing_tests PRIVATE tessera_harness)

add_test(NAME harness_framing COMMAND harness_framing_tests)

add_executable(harness_transport_tests
    transport_tests.cpp)

target_link_libraries(harness_transport_tests PRIVATE tessera_harness)

add_test(NAME harness_transport COMMAND harness_transport_tests)

add_executable(harness_json_tests
    json_tests.cpp)

target_link_libraries(harness_json_tests PRIVATE tessera_harness)

add_test(NAME harness_json COMMAND harness_json_tests)

add_executable(harness_protocol_tests
    protocol_tests.cpp)

target_link_libraries(harness_protocol_tests PRIVATE tessera_harness)

add_test(NAME harness_protocol COMMAND harness_protocol_tests)

add_executable(harness_command_tests
    command_tests.cpp)

target_link_libraries(harness_command_tests PRIVATE tessera_harness)

add_test(NAME harness_command COMMAND harness_command_tests)

add_executable(harness_command_result_tests
    command_result_tests.cpp)

target_link_libraries(harness_command_result_tests PRIVATE tessera_harness)

add_test(NAME harness_command_result COMMAND harness_command_result_tests)
add_executable(harness_golden_tests
    golden_bytes_tests.cpp)

target_link_libraries(harness_golden_tests PRIVATE tessera_harness)

add_test(NAME harness_golden COMMAND harness_golden_tests)

add_executable(harness_stdio_transport_tests
    stdio_transport_tests.cpp)

target_link_libraries(harness_stdio_transport_tests PRIVATE tessera_harness)

add_test(NAME harness_stdio_transport COMMAND harness_stdio_transport_tests)

add_executable(harness_handler_tests
    harness_handler_tests.cpp)

target_link_libraries(harness_handler_tests PRIVATE tessera_harness)

add_test(NAME harness_handler COMMAND harness_handler_tests)

add_executable(harness_session_tests
    session_tests.cpp)

target_link_libraries(harness_session_tests PRIVATE tessera_harness)

add_test(NAME harness_session COMMAND harness_session_tests)

add_executable(harness_cli_tests
    cli_tests.cpp)

target_link_libraries(harness_cli_tests PRIVATE tessera_harness)

add_test(NAME harness_cli COMMAND harness_cli_tests)