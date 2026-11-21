# SPDX-License-Identifier: MIT

add_library(tessera_frame STATIC
    frame_pipeline.cpp
    # The style-to-scene sync half of FramePipeline (issue #735, slice B): the whole-tree walk and
    # the scoped walk that visits only the restyled subtrees. Split out of frame_pipeline.cpp,
    # which is at its size limit.
    frame_style_sync.cpp
    interpolation.cpp
    paint_view.cpp)

target_include_directories(tessera_frame PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_frame PUBLIC
    tessera_animation_loop
    tessera_layout
    tessera_paint
    tessera_scene
    tessera_damage)

target_compile_features(tessera_frame PUBLIC cxx_std_23)
