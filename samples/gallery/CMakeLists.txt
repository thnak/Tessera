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

add_executable(gallery main.cpp golden.cpp golden.hpp ${_gallery_outputs})
target_include_directories(gallery PRIVATE ${_gallery_gen})
target_link_libraries(gallery PRIVATE
    tessera_app_window
    tessera_scene
    tessera_text_edit
    tessera_animation
    tessera_style
    tessera_harness_capture
    tessera_stb)
target_compile_definitions(gallery PRIVATE
    TESSERA_GALLERY_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf"
    TESSERA_GALLERY_BUTTON_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/button.css"
    TESSERA_GALLERY_CSS="${CMAKE_CURRENT_SOURCE_DIR}/gallery.css"
    TESSERA_GALLERY_THEME="${PROJECT_SOURCE_DIR}/packages/tessera-ui/themes/instrument.theme.json")
target_compile_features(gallery PRIVATE cxx_std_23)

if (TESSERA_BUILD_TESTS)
    enable_testing()
    # Smoke: one capture on any Vulkan device. No device: the tool prints its documented skip line.
    add_test(NAME gallery_capture
        COMMAND gallery --capture ${CMAKE_CURRENT_BINARY_DIR}/gallery_smoke.png)
    set_tests_properties(gallery_capture PROPERTIES SKIP_REGULAR_EXPRESSION "skipped")

    # Goldens (issue #239, ADR-009): every theme in every state, plus density, text scale and RTL
    # on the light theme. The committed baselines are lavapipe images under tests/golden/gallery/;
    # the test skips unless the loader selects only lavapipe. frame.png, the diff and the compare
    # report of every run land in the build tree (CI uploads them), so a missing or changed
    # baseline can be reviewed and committed. `gallery --golden FILE --update-goldens ...` rewrites.
    set(_gallery_golden_dir ${PROJECT_SOURCE_DIR}/tests/golden/gallery)
    set(_gallery_artifact_dir ${CMAKE_CURRENT_BINARY_DIR}/golden-artifacts)
    file(MAKE_DIRECTORY ${_gallery_artifact_dir})
    set(_gallery_cases
        "compact|--density compact"
        "scale_150|--text-scale 150"
        "rtl|--rtl")
    foreach (_theme light dark high_contrast)
        string(REPLACE "_" "-" _theme_id "${_theme}")
        foreach (_state none hover active focus)
            list(APPEND _gallery_cases "${_theme}_${_state}|--theme instrument-${_theme_id} --state ${_state}")
        endforeach ()
    endforeach ()
    foreach (_case IN LISTS _gallery_cases)
        string(REPLACE "|" ";" _parts "${_case}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _flags)
        separate_arguments(_flag_list NATIVE_COMMAND "${_flags}")
        add_test(NAME gallery_golden_${_name}
            COMMAND gallery --golden ${_gallery_golden_dir}/${_name}.png
                    --artifacts ${_name} ${_flag_list}
            WORKING_DIRECTORY ${_gallery_artifact_dir})
        set_tests_properties(gallery_golden_${_name} PROPERTIES SKIP_REGULAR_EXPRESSION "skipped")
    endforeach ()
endif ()
