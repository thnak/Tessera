# SPDX-License-Identifier: MIT

# Gallery skeleton (issue #239, ADR-047 item 10), native lane. The page is generated at build time
# from Gallery.tsx and the library Button by the host-built codegen CLI (host-only, like the
# compiler demo), so the gallery always shows the Button that ships.
set(_gallery_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(_gallery_button ${CMAKE_SOURCE_DIR}/packages/tessera-ui/Button.tsx)
set(_gallery_outputs
    ${_gallery_gen}/Button.hpp ${_gallery_gen}/Button.cpp
    ${_gallery_gen}/Gallery.hpp ${_gallery_gen}/Gallery.cpp)
add_custom_command(
    OUTPUT ${_gallery_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_gallery_gen}/src
    COMMAND ${CMAKE_COMMAND} -E copy ${_gallery_button} ${_gallery_gen}/src/Button.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/Gallery.tsx ${_gallery_gen}/src/Gallery.tsx
    COMMAND $<TARGET_FILE:tessera_codegen_cli> --mode legacy --write --out ${_gallery_gen}
            --namespace gallery --salt gallery --root ${_gallery_gen}/src
            ${_gallery_gen}/src/Button.tsx ${_gallery_gen}/src/Gallery.tsx
    DEPENDS tessera_codegen_cli ${_gallery_button} ${CMAKE_CURRENT_SOURCE_DIR}/Gallery.tsx
    COMMENT "Generating the gallery page"
    VERBATIM)

add_executable(gallery main.cpp ${_gallery_outputs})
target_include_directories(gallery PRIVATE ${_gallery_gen})
target_link_libraries(gallery PRIVATE
    tessera_app_window
    tessera_scene
    tessera_text_edit
    tessera_animation
    tessera_style
    tessera_stb)
target_compile_definitions(gallery PRIVATE
    TESSERA_GALLERY_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf"
    TESSERA_GALLERY_BUTTON_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/button.css"
    TESSERA_GALLERY_CSS="${CMAKE_CURRENT_SOURCE_DIR}/gallery.css"
    TESSERA_GALLERY_THEME="${PROJECT_SOURCE_DIR}/packages/tessera-ui/themes/instrument.theme.json")
target_compile_features(gallery PRIVATE cxx_std_23)

if (TESSERA_BUILD_TESTS)
    enable_testing()
    # One capture per theme, plus the compact, scaled, RTL and forced-state switches. No Vulkan
    # device: the tool prints its documented skip line and exits 0.
    set(_gallery_cases
        "light|--theme instrument-light"
        "dark|--theme instrument-dark"
        "high_contrast|--theme instrument-high-contrast"
        "compact|--density compact"
        "scale_150|--text-scale 150"
        "rtl|--rtl"
        "hover|--state hover"
        "active|--state active"
        "focus|--state focus")
    foreach (_case IN LISTS _gallery_cases)
        string(REPLACE "|" ";" _parts "${_case}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _flags)
        separate_arguments(_flag_list NATIVE_COMMAND "${_flags}")
        add_test(NAME gallery_capture_${_name}
            COMMAND gallery --capture ${CMAKE_CURRENT_BINARY_DIR}/gallery_${_name}.png ${_flag_list})
        set_tests_properties(gallery_capture_${_name} PROPERTIES SKIP_REGULAR_EXPRESSION "skipped")
    endforeach ()
endif ()
