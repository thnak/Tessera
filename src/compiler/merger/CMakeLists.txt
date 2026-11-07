# SPDX-License-Identifier: MIT

add_library(tessera_merger STATIC
    src/merger.cpp)

target_include_directories(tessera_merger PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(tessera_merger PUBLIC cxx_std_20)
