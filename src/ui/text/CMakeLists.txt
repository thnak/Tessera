# SPDX-License-Identifier: MIT

add_library(tessera_text STATIC
    font_metrics.cpp
    text_measure.cpp
    text_measure_cache.cpp)

target_include_directories(tessera_text PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_text PUBLIC tessera_style)

target_compile_features(tessera_text PUBLIC cxx_std_23)
