# SPDX-License-Identifier: MIT

# stylesheet_startup_benchmark (issue #201, ADR-048): the deterministic startup benchmark that runs
# one stylesheet through both startup paths -- the embedded blob decoded by load_stylesheet_blob()
# and the run-time parse+compile of compile_stylesheet() -- and compares them on stage counters
# (bytes read, rules, selectors, allocations). No wall clock and no timing gate, per
# docs/guides/testing.md section 7.
#
# The executable is built in every configuration so CI compiles it under warnings-as-errors. The
# ctest entry is opt-in: it carries the `benchmark` label and is DISABLED unless
# TESSERA_BENCHMARK_STYLESHEET=ON, so the default gating suite reports it as "Not Run (Disabled)"
# and never spends time on it. Enable and run it with:
#
#   cmake -DTESSERA_BENCHMARK_STYLESHEET=ON ...
#   ctest -L benchmark --output-on-failure
include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)

add_executable(tessera_stylesheet_benchmark
    stylesheet_benchmark.cpp
    benchmark_baseline.cpp
    benchmark_counters.cpp
    benchmark_paths.cpp
    benchmark_report.cpp)

# The reference stylesheet is embedded at build time, so both startup paths read identical bytes and
# the benchmark opens no stylesheet at run time.
tessera_embed_assets(tessera_stylesheet_benchmark
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/benchmark.manifest
    NAMESPACE tessera::benchmark::stylesheet
    STEM stylesheet_benchmark)

target_link_libraries(tessera_stylesheet_benchmark PRIVATE
    tessera_assets
    tessera_harness
    tessera_style)

option(TESSERA_BENCHMARK_STYLESHEET
    "Run the opt-in stylesheet startup benchmark in ctest (label: benchmark)" OFF)

add_test(
    NAME stylesheet_startup_benchmark
    COMMAND tessera_stylesheet_benchmark
        ${PROJECT_SOURCE_DIR}/tests/golden/perf/stylesheet_benchmark.json)

# A label alone does not keep a test out of a plain `ctest` run, and `DISABLED` cannot be overridden
# by `-L`, so the two are combined: the label is the opt-in handle and the generator expression is
# the gate. `ctest -L benchmark` after -DTESSERA_BENCHMARK_STYLESHEET=ON runs it.
set_tests_properties(stylesheet_startup_benchmark PROPERTIES
    LABELS benchmark
    DISABLED $<NOT:$<BOOL:${TESSERA_BENCHMARK_STYLESHEET}>>)
