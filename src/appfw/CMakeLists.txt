# SPDX-License-Identifier: MIT
#
# ADR-050 layer 1: `tessera_appfw_core`. AF1 (issue #173) starts the library with the service
# registry and the service state machine; `Application`, `TaskScope`, timers, `Observable` and the
# native surface host land in AF2-AF7 and are added to this target as they arrive.

add_library(tessera_appfw_core STATIC
    service_bridge.cpp
    service_context.cpp
    service_runner.cpp
    service_runner_sequence.cpp)

target_include_directories(tessera_appfw_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_appfw_core PUBLIC cxx_std_23)

# Layer 1 depends on layer 0 only (ADR-050 section 1). `tessera_runtime` provides the clock, the
# completion queue, cancellation and `Task`; nothing here names a platform API or a UI internal.
target_link_libraries(tessera_appfw_core
    PUBLIC tessera_runtime)

if (TESSERA_BUILD_TESTS)
    enable_testing()

    # The AF1 tests (ADR-050 section 4): deterministic, driven by `ManualClock` and the completion
    # queue, with no wall clock, no sleeps and no network.
    add_executable(appfw_service_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_service_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_service_context_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_service_runner_tests.cpp)
    target_link_libraries(appfw_service_tests PRIVATE tessera_appfw_core)
    add_test(NAME appfw_service COMMAND appfw_service_tests)
endif()
