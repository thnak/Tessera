# SPDX-License-Identifier: MIT

# The render-scale library lives in src/ui/scale; it is registered here until the root CMakeLists
# (Lead-owned) lists that directory directly. The text layer is its first consumer (per-scale glyph
# atlases), and relocating the add_subdirectory line later is a no-op for the target itself.
add_subdirectory(${PROJECT_SOURCE_DIR}/src/ui/scale ${CMAKE_BINARY_DIR}/src/ui/scale)

add_library(tessera_text STATIC
    bidi.cpp
    hit_test.cpp
    edit_buffer.cpp
    edit_grapheme.cpp
    font_face.cpp
    font_metrics.cpp
    freetype_metrics.cpp
    glyph_atlas.cpp
    glyph_raster.cpp
    scale_glyph_atlas.cpp
    harfbuzz_shaper.cpp
    text_measure.cpp
    text_measure_cache.cpp)

target_include_directories(tessera_text PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# Third-party headers stay private: public text headers must not include them.
target_link_libraries(tessera_text
    PUBLIC tessera_style tessera_scale
    PRIVATE tessera_freetype tessera_harfbuzz tessera_sheenbidi)

target_compile_features(tessera_text PUBLIC cxx_std_23)

add_subdirectory(render)

# Editing core on top of the text layer: the UTF-8 edit buffer plus the scene-event controller.
# Kept separate from tessera_text so the shaping/text layer stays free of scene dependencies.
add_library(tessera_text_edit STATIC
    caret_blink.cpp
    edit_controller.cpp
    edit_history.cpp
    text_input_runtime.cpp)

target_include_directories(tessera_text_edit PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_text_edit PUBLIC tessera_text tessera_scene tessera_hal_clipboard)

target_compile_features(tessera_text_edit PUBLIC cxx_std_23)