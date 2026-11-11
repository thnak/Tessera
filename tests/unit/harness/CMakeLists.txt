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

add_executable(harness_session_stream_tests
    session_stream_tests.cpp)

target_link_libraries(harness_session_stream_tests PRIVATE tessera_harness)

add_test(NAME harness_session_stream COMMAND harness_session_stream_tests)

add_executable(harness_cli_tests
    cli_tests.cpp)

target_link_libraries(harness_cli_tests PRIVATE tessera_harness)

add_test(NAME harness_cli COMMAND harness_cli_tests)

# Test-only fake HAL and the script runner/fake-HAL suites. `tessera_harness_testing` is built only
# for tests: no shipping target links it, and `fake_hal.hpp` states that in its first paragraph.
add_library(tessera_harness_testing STATIC
    fake_hal.cpp)

target_include_directories(tessera_harness_testing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tessera_harness_testing PUBLIC tessera_harness)

# `hal::Window` keeps one out-of-line default (`wait_for_events`) in the platform HAL target; the
# fake overrides it, but the base class's key function still has to resolve for a derived double.
target_link_libraries(tessera_harness_testing PRIVATE tessera_hal_window)

target_compile_features(tessera_harness_testing PUBLIC cxx_std_23)

add_executable(harness_fake_hal_tests
    fake_hal_tests.cpp)

target_link_libraries(harness_fake_hal_tests PRIVATE tessera_harness_testing)

add_test(NAME harness_fake_hal COMMAND harness_fake_hal_tests)

add_executable(harness_script_tests
    script_tests.cpp)

target_link_libraries(harness_script_tests PRIVATE tessera_harness)

add_test(NAME harness_script COMMAND harness_script_tests)

add_executable(harness_script_runner_tests
    script_runner_tests.cpp)

target_link_libraries(harness_script_runner_tests PRIVATE tessera_harness_testing)

add_test(NAME harness_script_runner COMMAND harness_script_runner_tests)
