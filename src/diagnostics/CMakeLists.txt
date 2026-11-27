# SPDX-License-Identifier: MIT

add_library(tessera_diagnostics STATIC
    crash.cpp
    crash_capture.cpp
    crash_redact.cpp
    crash_report.cpp
    crash_symbols.cpp
    crash_symbols_elf.cpp
    crash_symbols_map.cpp
    crash_upload.cpp
    log.cpp
    memory.cpp
    metrics.cpp
    offscreen_pressure.cpp
    overlay.cpp)

# Crash capture arms are mutually exclusive (SEH versus signals), so each source is listed only on
# its own platform. Listing both and relying on `#if` would compile an empty translation unit on the
# other lane, and GCC only warns about what it can see.
# Three cases, not two: Emscripten is neither. The POSIX arm includes <execinfo.h>, which Emscripten
# does not provide, and a signal handler is meaningless in a browser anyway -- there is no signal to
# catch and no address space to walk, and the module has no filesystem to write a report to. So
# Emscripten gets no capture arm and keeps the platform-neutral install/read surface, which reports
# capture as unsupported instead of failing to compile.
if(EMSCRIPTEN)
    # No capture arm by design; see the comment above.
elseif(WIN32)
    target_sources(tessera_diagnostics PRIVATE crash_capture_win.cpp)
else()
    target_sources(tessera_diagnostics PRIVATE crash_capture_posix.cpp)
endif()

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

    # Crash capture, symbolication and the opt-in uploader (#192,
    # docs/specs/crash-reporting-v1.md). Registered here for the same reason as frame_budget above:
    # the shared tests/unit registry is not this lane's to edit.
    #
    # `crash_capture` spawns a helper that really crashes (`crash_capture_helper`, below), so it is
    # only registered where the platform has the primitive: signals/`fork` on POSIX and
    # `CreateProcess` plus SEH on Windows. iOS/tvOS/watchOS have neither a forked child nor
    # `system()`, so the test would have nothing to run there.
    add_executable(crash_capture_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/crash_capture_tests.cpp)
    target_link_libraries(crash_capture_tests PRIVATE tessera_diagnostics)
    add_test(NAME crash_capture COMMAND crash_capture_tests $<TARGET_FILE:crash_capture_helper>
                                                    ${CMAKE_CURRENT_BINARY_DIR}/crash-capture-test)
    set_tests_properties(crash_capture PROPERTIES
        SKIP_REGULAR_EXPRESSION ": skipped"
        TIMEOUT 120)

    # A real crash, in a real child process, with the real handler armed. `WILL_FAIL` is wrong here:
    # the child is *supposed* to die, and the test asserts that it did and that a report exists.
    add_executable(crash_capture_helper
        ${PROJECT_SOURCE_DIR}/tests/unit/crash_capture_helper.cpp)
    target_link_libraries(crash_capture_helper PRIVATE tessera_diagnostics)
    # The helper must be able to crash on purpose. A Release build with /WX or -Werror is fine; it
    # is the *test* that reads the report, not the helper.
    set_target_properties(crash_capture_helper PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    # Symbolication against this build's own symbol artifact (#192). The test parses the running
    # binary and resolves a function it knows it contains, so it proves the symbol file matches the
    # image that produced the crash - not a fixture.
    #
    # The artifact differs per lane: ELF `.symtab` on Linux/macOS (kept by a non-stripped release),
    # and on Windows a linker `/MAP` file, because a Release build there embeds `/Z7` debug info in
    # the objects and emits no PDB (measured: 286 Release test executables, zero `.pdb`). So the
    # test gets the map as its second argument, produced by the /MAP option below. That option is
    # exactly what a Windows release lane must add to ship symbol files (see the report).
    add_executable(crash_symbols_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/crash_symbols_tests.cpp)
    target_link_libraries(crash_symbols_tests PRIVATE tessera_diagnostics)
    if(MSVC)
        # `/MAP:<file>` next to the executable. The generator expression keeps it in the same
        # per-configuration directory as the binary, so the test's second argument resolves.
        target_link_options(crash_symbols_tests PRIVATE
            "/MAP:$<TARGET_FILE_DIR:crash_symbols_tests>/crash_symbols_tests.map")
        add_test(NAME crash_symbols
            COMMAND crash_symbols_tests $<TARGET_FILE:crash_symbols_tests>
                    $<TARGET_FILE_DIR:crash_symbols_tests>/crash_symbols_tests.map)
    else()
        add_test(NAME crash_symbols COMMAND crash_symbols_tests $<TARGET_FILE:crash_symbols_tests>)
    endif()
    set_tests_properties(crash_symbols PROPERTIES
        SKIP_REGULAR_EXPRESSION ": skipped")

    # Uploader opt-in and redaction (#192): a recording transport proves zero sends when disabled,
    # and a planted user-name marker proves redaction.
    add_executable(crash_upload_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/crash_upload_tests.cpp)
    target_link_libraries(crash_upload_tests PRIVATE tessera_diagnostics)
    add_test(NAME crash_upload COMMAND crash_upload_tests)
endif()
