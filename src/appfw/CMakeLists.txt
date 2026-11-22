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
    saved_state.cpp
    service_bridge.cpp
    service_context.cpp
    service_runner.cpp
    service_runner_sequence.cpp
    task_scope.cpp)

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

# ADR-050 layer 1 module: `tessera_appfw_i18n` (AF8, issue #195). A separate static library for the
# same reason as storage: an app that does not adopt i18n links none of it. It depends on
# `tessera_appfw_core` for the service contract and `Observable<T>`, and on `tessera_text` for the
# measurement seam the locale-change dirty classification measures through - not on `tessera_layout`:
# a module must not reach into the UI layer (ADR-050 section 1), and the classification only needs
# `TextMeasureCache`, which is the text module's. `tessera_text` also pulls `tessera_style`, which is
# what a `ComputedStyle`-derived `TextStyle` needs; the module itself names neither.
add_library(tessera_appfw_i18n STATIC
    i18n_catalog.cpp
    i18n_locale.cpp
    i18n_service.cpp
    i18n_text.cpp)

target_include_directories(tessera_appfw_i18n PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_appfw_i18n PUBLIC cxx_std_23)

target_link_libraries(tessera_appfw_i18n
    PUBLIC tessera_appfw_core tessera_text)

# ADR-050 layer 1 module: `tessera_appfw_navigation` (issue #196). A separate static library for the
# same reason as storage and i18n: an app that does not adopt navigation links none of it.
#
# Dependencies, and why each is the narrowest one:
#   * `tessera_appfw_core` for `Observable<T>` (AF4's frame-boundary route value) and the service
#     contract. ADR-050 section 1 puts the route observable in the same layer as `Observable`, so this is
#     the module's only required sibling.
#   * `tessera_harness` for the JSON codec the restore snapshot is written in. It is a *leaf* library that
#     links nothing from the project (see `src/harness/CMakeLists.txt`), so depending on it does not reach
#     into the UI layer; the alternative was a second JSON parser inside layer 1, and `src/ui/style`
#     already links the same library for the same reason (the theme asset). It is PRIVATE because no
#     public header of this module names a harness type: `navigation_restore.hpp` exposes only the
#     module's own `std::string`-based shape.
#   * `tessera_appfw_storage` for the atomic write the restore snapshot persists through. It is PUBLIC
#     because `NavRestoreStore` takes a `Storage&` in its constructor, and a caller must be able to name
#     the type it passes.
#
# Deliberately *not* linked: `tessera_scene`. The `ui::UpdateScheduler` the CLI needs is a header-only
# dependency of the class layout (`Observable<T>` holds an `UpdateTicket`), and `Observable` already
# declares it; linking the scene library would pull the whole scene graph into an app that only wants to
# route. The observable's teardown does call `UpdateScheduler::detach`, so the final link needs the
# symbol from whichever target the app already has - `tessera_appfw_core`'s `Observable` tests link
# `tessera_scene` for exactly that reason, and this module's test target does too.
add_library(tessera_appfw_navigation STATIC
    navigation_codec.cpp
    navigation_deep_link.cpp
    navigation_match.cpp
    navigation_restore_decode.cpp
    navigation_restore_encode.cpp
    navigation_restore_store.cpp
    navigation_route.cpp
    navigation_stack.cpp)

target_include_directories(tessera_appfw_navigation PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_appfw_navigation PUBLIC cxx_std_23)

target_link_libraries(tessera_appfw_navigation
    PUBLIC tessera_appfw_core tessera_appfw_storage
    PRIVATE tessera_harness)

# ADR-050 layer 1 module: `tessera_appfw_net` (issue #197, docs/specs/networking-v1.md, ADR-052). A
# separate static library for the same reason as storage, i18n and navigation: an app that does not
# adopt networking links none of it, and nothing here pulls a socket into `tessera_appfw_core`.
#
# Dependencies, and why each is the narrowest one:
#   * `tessera_appfw_core` for the service contract and the layer-1 build boundary. It is PUBLIC
#     because a caller names the module's types next to a service.
#   * `tessera_runtime` for the four layer-0 seams the module is handed rather than inventing:
#     `CompletionQueue` (where a result is delivered), `IoTaskExecutor` (where a blocking attempt
#     runs, so the UI thread never does), `TimerQueue` (where the per-attempt deadline lives, so a
#     retry/backoff deadline is a timer and not a sleep) and `Clock` (`ManualClock` in tests).
#     `tessera_appfw_core` already links it PUBLIC, so naming it is documentation as much as
#     configuration.
#
# Deliberately *not* linked, and this is the whole point of the transport seam: no WinHTTP, no
# `winhttp.lib`, no libcurl, no OpenSSL, no platform TLS library. `net_transport.hpp` is the only
# place a back end plugs in, and every one of them needs `docs/specs/dependencies.md` entries first
# (ADR-052 item 5). The build therefore has no new dependency and the Wasm lane compiles this module
# unchanged - there is no POSIX header and no socket call anywhere in it.
add_library(tessera_appfw_net STATIC
    net_client.cpp
    net_client_hop.cpp
    net_client_impl.cpp
    net_fake.cpp
    net_redirect.cpp
    net_state.cpp
    net_types.cpp
    net_url.cpp)

target_include_directories(tessera_appfw_net PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_appfw_net PUBLIC cxx_std_23)

target_link_libraries(tessera_appfw_net
    PUBLIC tessera_appfw_core tessera_runtime)

# ADR-050 layer 1 module: `tessera_appfw_shell` (issue #450, docs/specs/platform-shell-v1.md,
# ADR-030). A separate static library for the same reason as storage, i18n, navigation and net: an
# app that does not adopt the shell links none of it.
#
# Dependencies, and why each is the narrowest one:
#   * `tessera_appfw_core` for the layer-1 build boundary and the service contract. It is PUBLIC
#     because a caller names the module's types next to a service.
#   * `tessera_runtime` for the three layer-0 seams the module is handed rather than inventing:
#     `CompletionQueue` (where a result is delivered), `CancellationToken` (how an operation is
#     cancelled) and `Clock` (`ManualClock` in tests, which is what makes `FakeShell` deterministic).
#     `tessera_appfw_core` already links it PUBLIC, so naming it is documentation as much as
#     configuration.
#
# Deliberately *not* linked, and this is the whole point of the backend seam: no `user32`, no
# `comdlg32`, no `ole32`, no `shell32`, no `shlobj`. `shell_backend.hpp` is the only place a platform
# plugs in, and the Win32 back end is a separate target (`tessera_appfw_shell_win32`) that only a
# Windows build links. The headless contract suite therefore compiles and runs on every lane,
# including lavapipe and the Wasm lane, with no platform library anywhere in it.
add_library(tessera_appfw_shell STATIC
    shell.cpp
    shell_fake.cpp
    shell_impl.cpp
    shell_impl_requests.cpp
    shell_path.cpp)

target_include_directories(tessera_appfw_shell PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_appfw_shell PUBLIC cxx_std_23)

target_link_libraries(tessera_appfw_shell
    PUBLIC tessera_appfw_core tessera_runtime)

# The Win32 back end (issue #450, section 1's Windows column). It is its own target so the platform
# libraries are linked only where they exist, and so a Linux or Wasm build never sees a `windows.h`.
# It depends on the module for the seam types and on `tessera_hal_clipboard` for the clipboard
# primitive that already exists (the HAL seam is where the `OpenClipboard` retry budget lives, and
# this back end must not grow a second copy of it).
if (WIN32)
    add_library(tessera_appfw_shell_win32 STATIC
        ${PROJECT_SOURCE_DIR}/src/platform/windows/shell_win32.cpp
        ${PROJECT_SOURCE_DIR}/src/platform/windows/shell_win32_dialogs.cpp)

    target_include_directories(tessera_appfw_shell_win32 PUBLIC
        ${PROJECT_SOURCE_DIR}/include)

    target_compile_features(tessera_appfw_shell_win32 PUBLIC cxx_std_23)

    target_link_libraries(tessera_appfw_shell_win32
        PUBLIC tessera_appfw_shell
        PRIVATE tessera_hal_clipboard comdlg32 ole32 shell32 user32)
endif()

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

    # appfw_observable_allocation.cpp replaces the global operator new/delete to count allocations;
    # see the helper's comment.
    tessera_allow_allocator_override(appfw_observable_tests)

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
    # AF2 (issue #174, ADR-050 section 5 item 1): `TaskScope`'s structured cancellation. The
    # acceptance here is the roadmap row's "closing a scope cancels and drains"; the timer wake
    # source it also names is layer 0 and is tested by `runtime_timer` in `src/runtime`.
    add_executable(appfw_task_scope_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_task_scope_main.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_task_scope_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_task_scope_suspension_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_task_scope_cancel_tests.cpp)
    target_include_directories(appfw_task_scope_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(appfw_task_scope_tests PRIVATE tessera_appfw_core)
    add_test(NAME appfw_task_scope COMMAND appfw_task_scope_tests)

    # AF8 (issue #195, docs/specs/i18n-v1.md): locale negotiation, the compiled message catalog, the
    # locale service's change signal, and the locale-change dirty classification. The relayout
    # assertion links `tessera_layout` because it drives the *real* Yoga measure callback with the
    # same text cache, so what it asserts is the box layout will produce, not a mock of it.
    add_executable(appfw_i18n_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_i18n_main.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_i18n_locale_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_i18n_catalog_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_i18n_service_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_i18n_relayout_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_i18n_layout_tests.cpp)
    target_include_directories(appfw_i18n_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(appfw_i18n_tests PRIVATE tessera_appfw_i18n tessera_layout)
    add_test(NAME appfw_i18n COMMAND appfw_i18n_tests)

    # Navigation (issue #196, docs/specs/navigation-v1.md): the four acceptance groups, one TU each, so a
    # failure names route, back, deep-link or restore. It links `tessera_scene` for `UpdateScheduler`,
    # which `Observable<T>` attaches a ticket to and calls `detach` on in its destructor - the same reason
    # the AF4 observable target links it. Everything else is deterministic: the route table is explicit
    # data, the deep links are strings, and a restore runs on `ManualIoExecutor` against an in-memory file
    # system, so no test in the suite reads a clock, an OS, a network or a real file.
    add_executable(appfw_navigation_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_navigation_main.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_navigation_route_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_navigation_back_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_navigation_deep_link_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_navigation_restore_tests.cpp)
    target_include_directories(appfw_navigation_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(appfw_navigation_tests PRIVATE
        tessera_appfw_navigation tessera_scene tessera_runtime)
    add_test(NAME appfw_navigation COMMAND appfw_navigation_tests)

    # Networking (issue #197, docs/specs/networking-v1.md): the contract suite of section 5, one TU
    # per acceptance group so a failure names the group. Deterministic end to end - `ManualClock` for
    # every instant, `ManualIoExecutor` for the I/O stage, `FakeNet` for the network, a hand-fired
    # `TimerQueue` for the deadline and a hand-drained `CompletionQueue` for delivery - so no test
    # opens a socket, reads a wall clock, sleeps or starts a thread. It links `tessera_runtime` for
    # `RuntimeLoop` (the "a pending request idles with zero presents" case drives the real loop) and
    # nothing else: there is no platform network library anywhere in this target.
    add_executable(appfw_net_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_net_main.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_net_state_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_net_error_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_net_redirect_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_net_thread_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_net_request_tests.cpp)
    target_include_directories(appfw_net_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(appfw_net_tests PRIVATE tessera_appfw_net tessera_runtime)
    add_test(NAME appfw_net COMMAND appfw_net_tests)

    # The shell contract suite (issue #450, docs/specs/platform-shell-v1.md section 9): one TU per
    # acceptance group, so a failure names the rule. Deterministic end to end - `ManualClock` for
    # every instant, `FakeShell` for the platform, a hand-drained `CompletionQueue` for delivery - so
    # no test opens a dialog, touches a real clipboard, reads a wall clock, sleeps, starts a thread or
    # touches the file system. It links `tessera_appfw_shell` and nothing platform-specific, which is
    # what lets the same suite run on Windows, Linux and the Wasm lane.
    add_executable(appfw_shell_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_shell_main.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_shell_path_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_shell_capability_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_shell_clipboard_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_shell_dialog_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_shell_drop_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/appfw_shell_task_tests.cpp)
    target_include_directories(appfw_shell_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit)
    target_link_libraries(appfw_shell_tests PRIVATE tessera_appfw_shell tessera_runtime)
    add_test(NAME appfw_shell COMMAND appfw_shell_tests)
endif()
