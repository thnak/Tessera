# SPDX-License-Identifier: MIT

add_library(tessera_diagnostics STATIC
    crash.cpp
    log.cpp
    memory.cpp
    metrics.cpp
    overlay.cpp)

# Span tracing is compiled out by default (root CMakeLists.txt, TESSERA_ENABLE_TRACING): the source
# is not even listed, so no recorder code, event ring or Chrome JSON encoder can reach a binary that
# did not ask for it. Enabling the option restores the real recorder.
if (TESSERA_ENABLE_TRACING)
    target_sources(tessera_diagnostics PRIVATE trace.cpp)
    target_compile_definitions(tessera_diagnostics PUBLIC TESSERA_ENABLE_TRACING=1)
endif()

target_include_directories(tessera_diagnostics PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_diagnostics PUBLIC cxx_std_23)

if(WIN32)
    target_link_libraries(tessera_diagnostics PRIVATE psapi)
endif()

# Frame-budget metrics (#183, docs/specs/chart-node-v1.md section 7): the recorder's registration,
# overrun rule and the overlay's budget line. Registered from this subdirectory so the lane does not
# edit the shared tests/unit registry while other lanes are working in it.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(frame_budget_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/frame_budget_tests.cpp)
    target_link_libraries(frame_budget_tests PRIVATE tessera_diagnostics)
    add_test(NAME frame_budget COMMAND frame_budget_tests)
endif()
