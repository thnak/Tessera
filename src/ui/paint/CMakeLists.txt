# SPDX-License-Identifier: MIT

add_library(tessera_paint STATIC
    paint_pass.cpp
    paint_properties.cpp
    paint_sinks.cpp)

target_include_directories(tessera_paint PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_paint PUBLIC
    tessera_scene
    tessera_style
    tessera_gpu
    tessera_text_render)

target_compile_features(tessera_paint PUBLIC cxx_std_23)
