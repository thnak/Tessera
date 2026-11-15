# SPDX-License-Identifier: MIT
#
# ADR-050 layer 1: `tessera_appfw_core`. AF1 (issue #173) starts the library with the service
# registry and the service state machine; AF3 (issue #175) adds `Application`, the app lifecycle
# state machine and the ordered lifecycle dispatch. `TaskScope`, timers, `Observable` and the native
# surface host land in AF2-AF7 and are added to this target as they arrive.

add_library(tessera_appfw_core STATIC
    application.cpp
    application_lifecycle_sink.cpp
    lifecycle_script.cpp
    service_bridge.cpp
    service_context.cpp
    service_runner.cpp
    service_runner_sequence.cpp)

target_include_directories(tessera_appfw_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_appfw_core PUBLIC cxx_std_23)

# Layer 1 depends on layer 0 only (ADR-050 section 1). `tessera_runtime` provides the clock, the
# completion queue, cancellation and `Task`. The lifecycle's platform surface is the normalized
# `hal::WindowEventType` enumerator and the header-only `app::LifecycleSink` seam, so no HAL library
# is linked: a headless app-framework build stays free of a window backend (and its platform libs).
target_link_libraries(tessera_appfw_core
    PUBLIC tessera_runtime)

# ADR-050 layer 1 module: `tessera_appfw_storage` (AF7, issue #179). It is a separate static library
# because ADR-050 section 1 rule 2 makes the layer split an enforced build boundary: an app that does
# not adopt storage links none of it, and the module's dependency on the HAL file-system seam cannot
# leak into `tessera_appfw_core`.
#

# The only target this module needs beyond layer 0 is `tessera_hal_filesystem`, which is the
# platform seam the write sequence calls (`fsync`/`FlushFileBuffers`, `rename`/`MoveFileEx`). It does
# not link a window backend, so a headless storage build (and its contract tests) stays free of X11,
# Wayland and Win32 window libraries.
add_library(tessera_appfw_storage STATIC
    storage.cpp
    storage_atomic.cpp
    storage_envelope.cpp
    storage_settings.cpp)

target_include_directories(tessera_appfw_storage PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_appfw_storage PUBLIC cxx_std_23)

# Layer 1 module rule (ADR-050 section 1): the module may depend on `appfw_core`, layer 0 and the
# HAL. It must not reach into the UI layer.
target_link_libraries(tessera_appfw_storage
    PUBLIC tessera_appfw_core
    PRIVATE tessera_hal_filesystem)

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

    # AF4 (issue #176, ADR-050 section 6): `Observable<T>` is a header-only template over the scene
    # module's `UpdateScheduler`, so this target links `tessera_scene` directly. The allocation test
    # is a separate TU on purpose: it replaces global `operator new`, which would count the other
    # tests' setup too.
    add_executable(appfw_observable_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_observable_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_observable_subscription_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_observable_allocation.cpp)
    target_include_directories(appfw_observable_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(appfw_observable_tests PRIVATE tessera_scene)
    add_test(NAME appfw_observable COMMAND appfw_observable_tests)
    # The AF3 tests (issue #175, ADR-050 section 2): the state table of section 2 row by row, the
    # ordered-event vector a transition produces, the termination gate and `Defer`, and the
    # lifecycle script replayed through the same state machine every platform lane runs.
    add_executable(appfw_lifecycle_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_lifecycle_main.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_lifecycle_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_application_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_lifecycle_script_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_lifecycle_seam_tests.cpp)
    target_link_libraries(appfw_lifecycle_tests PRIVATE tessera_appfw_core)
    target_compile_definitions(appfw_lifecycle_tests PRIVATE
        TESSERA_LIFECYCLE_SCRIPT_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/lifecycle")
    add_test(NAME appfw_lifecycle COMMAND appfw_lifecycle_tests)

    # AF7 (issue #179, ADR-050 section 8 row `storage`): the storage contract suite. It is
    # deterministic end to end - `ManualIoExecutor` for the executor seam, a fake `FileSystem` that
    # records the step order and can be made to fail at any single step, and a scratch directory
    # under this build tree for the one real-file test. No thread, no sleep, no wall clock, and no
    # `%TEMP%` path.
    add_executable(appfw_storage_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_storage_main.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_storage_atomic_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_storage_cancel_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_storage_corruption_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_storage_executor_tests.cpp)
    target_include_directories(appfw_storage_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(appfw_storage_tests PRIVATE tessera_appfw_storage tessera_runtime)
    target_compile_definitions(appfw_storage_tests PRIVATE
        TESSERA_STORAGE_SCRATCH_DIR="${CMAKE_BINARY_DIR}/storage-scratch")
    add_test(NAME appfw_storage COMMAND appfw_storage_tests)
endif()
