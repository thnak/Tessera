# SPDX-License-Identifier: MIT

# Render-scale/DPI quantization tests; the slice registers its own executables and ctest names
# here so no two slices edit `tests/unit/CMakeLists.txt` at the same time.

add_executable(scale_quantize_tests
    scale_quantize_tests.cpp)

target_link_libraries(scale_quantize_tests PRIVATE tessera_scale)

add_test(NAME scale_quantize COMMAND scale_quantize_tests)

# The per-window AA policy resolver (display-aware-rendering.md): pure CPU, no window or GPU.
add_executable(aa_policy_tests
    aa_policy_tests.cpp)

target_link_libraries(aa_policy_tests PRIVATE tessera_scale)

add_test(NAME aa_policy COMMAND aa_policy_tests)

add_executable(scale_state_tests
    scale_state_tests.cpp)

target_link_libraries(scale_state_tests PRIVATE tessera_scale)

add_test(NAME scale_state COMMAND scale_state_tests)

# The per-scale atlas set is exercised against the committed fixture font; TESSERA_FONT_FIXTURE is
# defined by the parent tests/unit directory and resolved to an absolute path there.
add_executable(scale_atlas_tests
    scale_atlas_tests.cpp)

target_link_libraries(scale_atlas_tests PRIVATE tessera_text)

add_test(NAME scale_atlas COMMAND scale_atlas_tests "${TESSERA_FONT_FIXTURE}")

# Text consumes the AA policy: hinting by scale and quarter-pixel glyph origins
# (display-aware-rendering.md S5 and S6). The fixture font is resolved by the parent directory.
add_executable(text_aa_policy_tests
    text_aa_policy_tests.cpp)

target_link_libraries(text_aa_policy_tests PRIVATE tessera_text_render tessera_rhi_null)

add_test(NAME text_aa_policy COMMAND text_aa_policy_tests "${TESSERA_FONT_FIXTURE}")

# TextRenderer's atlas-per-ladder-step contract. The null device keeps the draw records
# deterministic, so no GPU/lavapipe is needed.
add_executable(text_renderer_scale_tests
    text_renderer_scale_tests.cpp)

target_link_libraries(text_renderer_scale_tests PRIVATE tessera_text_render tessera_rhi_null)

add_test(NAME text_renderer_scale COMMAND text_renderer_scale_tests "${TESSERA_FONT_FIXTURE}")

# Text raster at device size (display-aware-rendering.md T1): the bitmap is rasterized at
# css_px * render_scale while the caller's metrics and quads stay in CSS. The placement math is
# internal to the text renderer, so the test includes its directory.
add_executable(text_device_raster_tests
    text_device_raster_tests.cpp)

target_include_directories(text_device_raster_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/ui/text/render)

target_link_libraries(text_device_raster_tests PRIVATE tessera_text_render tessera_rhi_null)

add_test(NAME text_device_raster COMMAND text_device_raster_tests "${TESSERA_FONT_FIXTURE}")

# The text coverage-contrast LUT (display-aware-rendering.md section 3.5, D2 amendment): a
# header-only table, so this needs no library beyond the include path.
add_executable(text_contrast_lut_tests
    text_contrast_lut_tests.cpp)

target_include_directories(text_contrast_lut_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/ui/text/render)

add_test(NAME text_contrast_lut COMMAND text_contrast_lut_tests)

# The logical-to-device frame transform (display-aware-rendering.md section 6): the composition of
# the device scale with the adaptive factor, and the proof that the CPU clip scissor follows it into
# target pixels. The null device records the scissor each draw ran with, so no GPU is needed.
add_executable(content_scale_tests
    content_scale_tests.cpp)

target_link_libraries(content_scale_tests PRIVATE
    tessera_gpu
    tessera_rhi_null
    tessera_scale)

add_test(NAME content_scale COMMAND content_scale_tests)
