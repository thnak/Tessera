# SPDX-License-Identifier: MIT

add_library(tessera_rhi INTERFACE)

target_include_directories(tessera_rhi INTERFACE
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_rhi INTERFACE cxx_std_23)
