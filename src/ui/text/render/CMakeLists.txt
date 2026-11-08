# SPDX-License-Identifier: MIT

add_library(tessera_text_render STATIC
    text_renderer.cpp)

target_include_directories(tessera_text_render PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_text_render PUBLIC
    tessera_text
    tessera_gpu)

target_compile_features(tessera_text_render PUBLIC cxx_std_23)
