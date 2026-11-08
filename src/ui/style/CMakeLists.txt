# SPDX-License-Identifier: MIT

add_library(tessera_style STATIC
    parser.cpp
    tokenizer.cpp)

target_include_directories(tessera_style PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_style PUBLIC cxx_std_23)
