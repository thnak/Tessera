# SPDX-License-Identifier: MIT

add_library(tessera_hal_timer STATIC
    steady_time.cpp)

target_include_directories(tessera_hal_timer PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_hal_timer PUBLIC cxx_std_23)
