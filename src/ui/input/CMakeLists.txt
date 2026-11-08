# SPDX-License-Identifier: MIT

add_library(tessera_input STATIC
    input_queue.cpp)

target_include_directories(tessera_input PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_input PUBLIC cxx_std_23)
