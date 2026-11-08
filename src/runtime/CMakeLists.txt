# SPDX-License-Identifier: MIT

add_library(tessera_runtime STATIC
    clock.cpp
    fixed_step.cpp)

target_include_directories(tessera_runtime PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_runtime PUBLIC cxx_std_23)
