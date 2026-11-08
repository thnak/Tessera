# SPDX-License-Identifier: MIT

add_library(tessera_diagnostics STATIC
    log.cpp
    metrics.cpp)

target_include_directories(tessera_diagnostics PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_diagnostics PUBLIC cxx_std_23)
