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

# Replaces the global operator new/delete to count allocations; see the helper's comment.
tessera_allow_allocator_override(memory_sizes_probe)

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
    # The budget is recorded from a Release build, and the gate itself refuses to compare across
    # configurations (sizeof and allocation counts differ). The sanitizer presets are RelWithDebInfo
    # with instrumentation, so they exclude the label (CMakePresets.json) rather than re-record a
    # budget that would then be wrong for the gating lanes.
    set_tests_properties(memory_budget PROPERTIES LABELS uninstrumented_baseline)
elseif (NOT APPLE)
    # A missing interpreter is reported, not silently skipped: a gate that does not run is worse
    # than a gate that fails.
    message(WARNING "Python3 not found: the `memory_budget` CTest is not registered")
endif()

# gpu_memory_probe (issue #703): the GPU half of the memory budget. Separate from
# memory_sizes_probe because that one deliberately links no RHI target, so it runs on every lane;
# this one needs a real rhi::Device. The category bytes it prints are our own accounting (extent x
# texel size, buffer size), not the driver's block sizes, so lavapipe and a real GPU agree and the
# row can be ratcheted.
add_executable(gpu_memory_probe
    gpu_memory_probe.cpp)

target_link_libraries(gpu_memory_probe PRIVATE tessera_rhi_null)
if (TARGET tessera_rhi_vulkan)
    target_link_libraries(gpu_memory_probe PRIVATE tessera_rhi_vulkan)
    target_compile_definitions(gpu_memory_probe PRIVATE TESSERA_PROBE_HAS_VULKAN)
endif()

target_compile_features(gpu_memory_probe PRIVATE cxx_std_23)

# The probe itself, so a probe that stops building or printing is caught on every lane that builds.
add_test(NAME gpu_memory_sizes COMMAND gpu_memory_probe --backend null)

# cache_probe (issue #706): measures whether the shaped-run and pipeline caches the memory-model
# audit could not find are actually needed. A measurement tool, not a CTest entry: it asserts
# nothing until the numbers have been read and the decision recorded.
add_executable(cache_probe
    cache_probe.cpp)

target_link_libraries(cache_probe PRIVATE tessera_text)
target_compile_features(cache_probe PRIVATE cxx_std_23)

# frame_alloc_probe (issue #701, step 1): allocations per steady-state frame. A measurement tool, not a
# CTest entry: it asserts nothing until the numbers have been read and a bound has been decided.
add_executable(frame_alloc_probe
    frame_alloc_probe.cpp)

target_link_libraries(frame_alloc_probe PRIVATE
    tessera_app
    tessera_runtime)

target_compile_features(frame_alloc_probe PRIVATE cxx_std_23)

# Replaces the global operator new/delete to count allocations; see the helper's comment.
tessera_allow_allocator_override(frame_alloc_probe)

# `--sites` resolves an allocation's return address to a symbol with `dladdr`, which needs the
# executable's own symbols in the dynamic table. On Windows DbgHelp reads the PDB instead.
if (UNIX AND NOT APPLE)
    target_link_options(frame_alloc_probe PRIVATE -rdynamic)
elseif (APPLE)
    target_link_options(frame_alloc_probe PRIVATE -Wl,-export_dynamic)
elseif (MSVC)
    # DbgHelp names a frame from the PDB; a Release link has none unless asked. Public symbols are
    # enough for function names, and the probe is a tool, so the PDB costs nothing that matters.
    target_link_options(frame_alloc_probe PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# alloc_per_frame (issue #701, step 4): the same measurement as the probe, with a bound attached.
# Three scenes (card grid, scrolling list, animated), each asserting that an idle frame allocates
# zero and that a change frame stays at or below its recorded budget. The budgets are the numbers
# measured when the test landed; `scripts/check_alloc_budget.py --update` lowers them, never raises.
add_executable(alloc_per_frame_tests
    alloc_per_frame_tests.cpp)

target_link_libraries(alloc_per_frame_tests PRIVATE
    tessera_app
    tessera_runtime)

target_compile_features(alloc_per_frame_tests PRIVATE cxx_std_23)

# Replaces the global operator new/delete to count allocations; see the helper's comment.
tessera_allow_allocator_override(alloc_per_frame_tests)

add_test(NAME alloc_per_frame COMMAND alloc_per_frame_tests)

# The ratchet over those numbers (issue #701 step 4). Registered here rather than in
# `scripts/gates.py` for the same reason `memory_budget` is: the aggregator's release-gates job has
# no build tree, so a gate that needs a compiled binary cannot live there. A lane that builds also
# tests.
if (Python3_EXECUTABLE AND NOT APPLE)
    add_test(
        NAME alloc_budget
        COMMAND "${Python3_EXECUTABLE}" -B
            "${PROJECT_SOURCE_DIR}/scripts/check_alloc_budget.py"
            --check
            --build-dir "${CMAKE_BINARY_DIR}")
    # The budget is recorded from a Release build, and the gate refuses to compare across
    # configurations, so this runs only where the numbers were measured.
    set_tests_properties(alloc_budget PROPERTIES LABELS uninstrumented_baseline)
endif()
