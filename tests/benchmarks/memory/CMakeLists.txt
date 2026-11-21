# SPDX-License-Identifier: MIT

# memory_sizes_probe (issue #13): the one place that prints every budgeted memory quantity as
# `name=value` for scripts/check_memory_budget.py.
#
# Two CTest entries, because they answer different questions and belong to different runners:
#
# * `memory_sizes` runs the probe itself. It needs nothing but the binary, so it is cheap and it
#   catches a probe that stops building, crashes, or stops printing a row -- the failure mode that
#   would otherwise make the Python gate report a missing measurement on every lane.
# * `memory_budget` runs `scripts/check_memory_budget.py --check --sizes-only` against the committed
#   budget. It is registered here, not in scripts/gates.py, for the same reason the preview budget
#   runs in `pages.yml` rather than in the aggregator: the aggregator's release-gates job has no
#   build tree (it is a checkout, a clang-format install and `python scripts/gates.py`), so a gate
#   that needs a compiled probe cannot live there. A lane that builds also tests; `--sizes-only`
#   keeps it to the deterministic rows, so it does not gate on resident memory.
#
# No GPU, no window, no font, no clock.
add_executable(memory_sizes_probe
    memory_sizes_probe.cpp)

target_link_libraries(memory_sizes_probe PRIVATE
    tessera_diagnostics
    tessera_input
    tessera_scene
    tessera_style)

target_compile_features(memory_sizes_probe PRIVATE cxx_std_23)

# The build configuration, so the gate can tell a Release measurement from a Debug one. It matters:
# a checked-STL Debug build has a different sizeof(Node) (320 vs 312 on MSVC), a larger PaintData
# and one extra proxy allocation per vector, so a Debug number is a *different claim* from the
# committed Release budget. The gate records one configuration and enforces only against it rather
# than silently comparing two different builds. `$<CONFIG>` is the build type on a single-config
# generator and the active configuration on a multi-config one.
target_compile_definitions(memory_sizes_probe PRIVATE
    TESSERA_MEMORY_PROBE_CONFIG="$<CONFIG>")

# The node count and idle cycle count are the same defaults the memory goldens use
# (tests/golden/memory/steady_state.txt), passed explicitly so a bare invocation of the binary from
# a build tree measures the same tree the gate does.
add_test(
    NAME memory_sizes
    COMMAND memory_sizes_probe --nodes 20000 --idle-cycles 300)

# `find_package(Python3)` is called in tests/gpu and tests/gpu/backends, and a find_package result is
# directory-scoped: `Python3_EXECUTABLE` set there is not visible here. Call it again (idempotent, the
# cache holds it) rather than depending on subdirectory ordering.
if (TESSERA_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
endif()

# The budget pins the exact sizeof of three hot types (computed style, paint data, paint slot) as
# measured with MSVC and libstdc++. Apple builds against libc++, whose standard types are smaller, so
# those rows measure 16 bytes lower there and the two-sided ratchet reports a stale budget. That is an
# ABI difference, not a regression, and one budget cannot hold two ABIs; the Apple lanes still run the
# `memory_sizes` probe above, so a probe that stops building or printing is still caught.
if (Python3_EXECUTABLE AND NOT APPLE)
    add_test(
        NAME memory_budget
        COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/scripts/check_memory_budget.py"
            --check
            --sizes-only
            --build-dir "${CMAKE_BINARY_DIR}")
elseif (NOT APPLE)
    # A missing interpreter is reported, not silently skipped: a gate that does not run is worse
    # than a gate that fails.
    message(WARNING "Python3 not found: the `memory_budget` CTest is not registered")
endif()

# frame_alloc_probe (issue #701, step 1): allocations per steady-state frame. A measurement tool, not a
# CTest entry: it asserts nothing until the numbers have been read and a bound has been decided.
add_executable(frame_alloc_probe
    frame_alloc_probe.cpp)

target_link_libraries(frame_alloc_probe PRIVATE
    tessera_app
    tessera_runtime)

target_compile_features(frame_alloc_probe PRIVATE cxx_std_23)
