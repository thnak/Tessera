# SPDX-License-Identifier: MIT

add_library(tessera_rhi INTERFACE)

target_include_directories(tessera_rhi INTERFACE
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_rhi INTERFACE cxx_std_23)

# Backend-independent RHI machinery shared by more than one backend (ADR-053 decision 2). These
# live under src/ because they are implementation, not contract: the public headers in
# include/tessera/rendering/rhi/** stay free of them.
add_library(tessera_rhi_shared STATIC
    retire_queue.cpp)

target_include_directories(tessera_rhi_shared PUBLIC
    ${PROJECT_SOURCE_DIR}/src/rendering/rhi)

target_link_libraries(tessera_rhi_shared PUBLIC tessera_rhi)

target_compile_features(tessera_rhi_shared PUBLIC cxx_std_23)
