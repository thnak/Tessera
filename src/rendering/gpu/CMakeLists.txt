# SPDX-License-Identifier: MIT

add_library(tessera_gpu STATIC
    batched2d_renderer.cpp)

target_include_directories(tessera_gpu PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_gpu PUBLIC tessera_rhi)

target_compile_features(tessera_gpu PUBLIC cxx_std_20)
