# SPDX-License-Identifier: MIT

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
    fallback_shaper.cpp
    harfbuzz_shaper.cpp
    text_breaks.cpp
    text_measure.cpp
    text_measure_cache.cpp
    text_paint.cpp
    text_wrap.cpp)

target_include_directories(tessera_text PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# Third-party headers stay private: public text headers must not include them.
target_link_libraries(tessera_text
    PUBLIC tessera_style tessera_scale
    PRIVATE tessera_freetype tessera_harfbuzz tessera_sheenbidi)

target_compile_features(tessera_text PUBLIC cxx_std_23)

# ICU integration (docs/design/icu-data.md): static ICU plus the linked reduced data blob, and the
# runtime that installs the blob as common data. Off by default, so the default build and CI are
# unchanged; when on, the text module also carries the ICU-backed segmentation and the locale
# resolver, and the module tests self-register from tests/unit/icu.
#
# The ICU module tests are registered canonically from tests/unit/CMakeLists.txt, guarded by the
# same option (this module is processed before the root calls enable_testing()).
if (TESSERA_ICU_REDUCED_DATA)
    target_sources(tessera_text PRIVATE icu_runtime.cpp locale_resolver.cpp)
    target_link_libraries(tessera_text PRIVATE tessera_icu)
    target_compile_definitions(tessera_text PUBLIC TESSERA_ICU_ENABLED=1)
endif()

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