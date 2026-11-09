# SPDX-License-Identifier: MIT

add_library(tessera_frame STATIC
    frame_pipeline.cpp)

target_include_directories(tessera_frame PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_frame PUBLIC
    tessera_animation_loop
    tessera_layout
    tessera_paint
    tessera_scene
    tessera_damage)

target_compile_features(tessera_frame PUBLIC cxx_std_23)
