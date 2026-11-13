# SPDX-License-Identifier: MIT

# tessera_benchmark (issue #241): the GPU-free CPU benchmark. It builds the deterministic
# 12 000-node reference scene, drives the production headless frame path, paints into draw lists
# through the null RHI, and compares median/p95 times against the committed budget
# (tests/golden/perf/cpu_benchmark.txt). No GPU, window, font, network or clock-dependent output.
include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)

add_executable(tessera_benchmark
    tessera_benchmark.cpp
    benchmark_driver.cpp
    benchmark_fixture.cpp
    benchmark_scene.cpp
    benchmark_budget.cpp)

# The raster-image fixture bytes are embedded at build time (one small PNG shared by every image
# node), so the benchmark needs no runtime filesystem and no third-party dependency.
tessera_embed_assets(tessera_benchmark
    MANIFEST ${PROJECT_SOURCE_DIR}/tests/fixtures/assets/images.manifest
    NAMESPACE tessera::benchmark::fixtures
    STEM cpu_benchmark)

target_link_libraries(tessera_benchmark PRIVATE
    tessera_app
    tessera_virtual
    tessera_paint
    tessera_gpu
    tessera_rhi_null
    tessera_image
    tessera_asset_image
    tessera_style
    tessera_scene
    tessera_runtime)

add_test(
    NAME cpu_benchmark
    COMMAND tessera_benchmark
        ${PROJECT_SOURCE_DIR}/tests/golden/perf/cpu_benchmark.txt)
