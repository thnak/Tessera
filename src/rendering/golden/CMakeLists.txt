# SPDX-License-Identifier: MIT

add_library(tessera_golden STATIC
    difference_class.cpp
    image_diff.cpp
    parity.cpp
    perceptual.cpp)

target_include_directories(tessera_golden PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_golden PUBLIC cxx_std_23)
