# SPDX-License-Identifier: MIT

# Gallery skeleton (issue #239, ADR-047 item 10), native lane. The page is generated at build time
# from Gallery.tsx and the library Button by the host-built codegen CLI (host-only, like the
# compiler demo), so the gallery always shows the Button that ships.
#
# One codegen path (ADR-040, issue #219): the CLI writes the generated `<Component>.g.hpp` bases and
# scaffolds the developer `<Component>.hpp/.cpp` classes once. Every component here has no `bind::`
# region, so the scaffolded classes are empty; the page drives them through the base API
# (`build`/`mount`/`attach_scheduler`).
set(_gallery_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(_gallery_button ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Button.tsx)
set(_gallery_safe_area ${PROJECT_SOURCE_DIR}/packages/tessera-ui/SafeArea.tsx)
set(_gallery_icon ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Icon.tsx)
set(_gallery_controls
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Switch.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Checkbox.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Radio.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/RadioGroup.tsx)
set(_gallery_display
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Badge.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Divider.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Card.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Avatar.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Skeleton.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Spinner.tsx)
set(_gallery_tags
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Range.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/ProgressBar.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Meter.tsx
    ${PROJECT_SOURCE_DIR}/packages/tessera-ui/Details.tsx)
set(_gallery_primitives ${PROJECT_SOURCE_DIR}/packages/tessera-ui/primitives/Progress.tsx)
set(_gallery_icon_glyph ${PROJECT_SOURCE_DIR}/packages/tessera-ui/icons/IconGlyph.tsx)
# The Settings starter template lives in samples/settings (issues #240, #527) and builds only on the
# library components above; the gallery runs it as `--page settings`.
set(_gallery_settings_page ${PROJECT_SOURCE_DIR}/samples/settings/SettingsPage.tsx)
set(_gallery_settings_css ${PROJECT_SOURCE_DIR}/samples/settings/settings.css)
# The codegen CLI as a command. In an Emscripten build it is the Node tool (NODERAWFS, see the
# compiler's CMakeLists), so it runs through node: the emsdk's (`EMSDK_NODE`, which emsdk_env.sh
# exports and CI sets) or the one on the PATH.
set(_gallery_codegen $<TARGET_FILE:tessera_codegen_cli>)
if (EMSCRIPTEN)
    if (DEFINED ENV{EMSDK_NODE})
        set(_gallery_node "$ENV{EMSDK_NODE}")
    else ()
        find_program(_gallery_node NAMES node REQUIRED)
    endif ()
    set(_gallery_codegen ${_gallery_node} $<TARGET_FILE:tessera_codegen_cli>)
endif ()
set(_gallery_outputs
    ${_gallery_gen}/Button.g.hpp ${_gallery_gen}/Button.hpp ${_gallery_gen}/Button.cpp
    ${_gallery_gen}/Gallery.g.hpp ${_gallery_gen}/Gallery.hpp ${_gallery_gen}/Gallery.cpp
    ${_gallery_gen}/SafeArea.g.hpp ${_gallery_gen}/SafeArea.hpp ${_gallery_gen}/SafeArea.cpp
    ${_gallery_gen}/SafeAreaPage.g.hpp ${_gallery_gen}/SafeAreaPage.hpp
    ${_gallery_gen}/SafeAreaPage.cpp
    ${_gallery_gen}/FormPage.g.hpp ${_gallery_gen}/FormPage.hpp ${_gallery_gen}/FormPage.cpp
    ${_gallery_gen}/DashPage.g.hpp ${_gallery_gen}/DashPage.hpp ${_gallery_gen}/DashPage.cpp
    ${_gallery_gen}/SwatchPage.g.hpp ${_gallery_gen}/SwatchPage.hpp ${_gallery_gen}/SwatchPage.cpp
    ${_gallery_gen}/Icon.g.hpp ${_gallery_gen}/Icon.hpp ${_gallery_gen}/Icon.cpp
    ${_gallery_gen}/IconGlyph.g.hpp ${_gallery_gen}/IconGlyph.hpp ${_gallery_gen}/IconGlyph.cpp
    ${_gallery_gen}/IconsPage.g.hpp ${_gallery_gen}/IconsPage.hpp ${_gallery_gen}/IconsPage.cpp
    ${_gallery_gen}/ListsPage.g.hpp ${_gallery_gen}/ListsPage.hpp ${_gallery_gen}/ListsPage.cpp
    ${_gallery_gen}/ControlsPage.g.hpp ${_gallery_gen}/ControlsPage.hpp
    ${_gallery_gen}/ControlsPage.cpp
    ${_gallery_gen}/Switch.g.hpp ${_gallery_gen}/Switch.hpp ${_gallery_gen}/Switch.cpp
    ${_gallery_gen}/Checkbox.g.hpp ${_gallery_gen}/Checkbox.hpp ${_gallery_gen}/Checkbox.cpp
    ${_gallery_gen}/Radio.g.hpp ${_gallery_gen}/Radio.hpp ${_gallery_gen}/Radio.cpp
    ${_gallery_gen}/RadioGroup.g.hpp ${_gallery_gen}/RadioGroup.hpp
    ${_gallery_gen}/RadioGroup.cpp
    ${_gallery_gen}/TagsPage.g.hpp ${_gallery_gen}/TagsPage.hpp ${_gallery_gen}/TagsPage.cpp
    ${_gallery_gen}/Range.g.hpp ${_gallery_gen}/Range.hpp ${_gallery_gen}/Range.cpp
    ${_gallery_gen}/ProgressBar.g.hpp ${_gallery_gen}/ProgressBar.hpp
    ${_gallery_gen}/ProgressBar.cpp
    ${_gallery_gen}/Meter.g.hpp ${_gallery_gen}/Meter.hpp ${_gallery_gen}/Meter.cpp
    ${_gallery_gen}/Details.g.hpp ${_gallery_gen}/Details.hpp ${_gallery_gen}/Details.cpp
    ${_gallery_gen}/Progress.g.hpp ${_gallery_gen}/Progress.hpp ${_gallery_gen}/Progress.cpp
    ${_gallery_gen}/TablesPage.g.hpp ${_gallery_gen}/TablesPage.hpp ${_gallery_gen}/TablesPage.cpp
    ${_gallery_gen}/SettingsPage.g.hpp ${_gallery_gen}/SettingsPage.hpp
    ${_gallery_gen}/SettingsPage.cpp
    ${_gallery_gen}/DisplayPage.g.hpp ${_gallery_gen}/DisplayPage.hpp ${_gallery_gen}/DisplayPage.cpp
    ${_gallery_gen}/Badge.g.hpp ${_gallery_gen}/Badge.hpp ${_gallery_gen}/Badge.cpp
    ${_gallery_gen}/Divider.g.hpp ${_gallery_gen}/Divider.hpp ${_gallery_gen}/Divider.cpp
    ${_gallery_gen}/Card.g.hpp ${_gallery_gen}/Card.hpp ${_gallery_gen}/Card.cpp
    ${_gallery_gen}/Avatar.g.hpp ${_gallery_gen}/Avatar.hpp ${_gallery_gen}/Avatar.cpp
    ${_gallery_gen}/Skeleton.g.hpp ${_gallery_gen}/Skeleton.hpp ${_gallery_gen}/Skeleton.cpp
    ${_gallery_gen}/Spinner.g.hpp ${_gallery_gen}/Spinner.hpp ${_gallery_gen}/Spinner.cpp)
add_custom_command(
    OUTPUT ${_gallery_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_gallery_gen}/src
    COMMAND ${CMAKE_COMMAND} -E copy ${_gallery_button} ${_gallery_gen}/src/Button.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/Gallery.tsx ${_gallery_gen}/src/Gallery.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${_gallery_safe_area} ${_gallery_gen}/src/SafeArea.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/SafeAreaPage.tsx ${_gallery_gen}/src/SafeAreaPage.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/FormPage.tsx ${_gallery_gen}/src/FormPage.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/form.module.css ${_gallery_gen}/src/form.module.css
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/DashPage.tsx ${_gallery_gen}/src/DashPage.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/SwatchPage.tsx ${_gallery_gen}/src/SwatchPage.tsx
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_gallery_gen}/src/icons
    COMMAND ${CMAKE_COMMAND} -E copy ${_gallery_icon} ${_gallery_gen}/src/Icon.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${_gallery_icon_glyph} ${_gallery_gen}/src/icons/IconGlyph.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/IconsPage.tsx ${_gallery_gen}/src/IconsPage.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/ListsPage.tsx ${_gallery_gen}/src/ListsPage.tsx
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_gallery_controls} ${_gallery_gen}/src
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/ControlsPage.tsx ${_gallery_gen}/src/ControlsPage.tsx
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/TablesPage.tsx ${_gallery_gen}/src/TablesPage.tsx
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_gallery_tags} ${_gallery_gen}/src
    COMMAND ${CMAKE_COMMAND} -E copy ${_gallery_settings_page} ${_gallery_gen}/src/SettingsPage.tsx
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_gallery_display} ${_gallery_gen}/src
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/DisplayPage.tsx ${_gallery_gen}/src/DisplayPage.tsx
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_gallery_gen}/src/primitives
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_gallery_primitives} ${_gallery_gen}/src/primitives
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/TagsPage.tsx ${_gallery_gen}/src/TagsPage.tsx
    COMMAND ${_gallery_codegen} --write --out ${_gallery_gen}
            --namespace gallery --salt gallery --root ${_gallery_gen}/src
            ${_gallery_gen}/src/Button.tsx ${_gallery_gen}/src/Gallery.tsx
            ${_gallery_gen}/src/SafeArea.tsx ${_gallery_gen}/src/SafeAreaPage.tsx
            ${_gallery_gen}/src/FormPage.tsx ${_gallery_gen}/src/DashPage.tsx
            ${_gallery_gen}/src/SwatchPage.tsx ${_gallery_gen}/src/Icon.tsx
            ${_gallery_gen}/src/icons/IconGlyph.tsx ${_gallery_gen}/src/IconsPage.tsx
            ${_gallery_gen}/src/ListsPage.tsx ${_gallery_gen}/src/ControlsPage.tsx
            ${_gallery_gen}/src/Switch.tsx ${_gallery_gen}/src/Checkbox.tsx
            ${_gallery_gen}/src/Radio.tsx ${_gallery_gen}/src/RadioGroup.tsx ${_gallery_gen}/src/TablesPage.tsx
            ${_gallery_gen}/src/TagsPage.tsx ${_gallery_gen}/src/Range.tsx
            ${_gallery_gen}/src/ProgressBar.tsx ${_gallery_gen}/src/Meter.tsx
            ${_gallery_gen}/src/Details.tsx ${_gallery_gen}/src/primitives/Progress.tsx
            ${_gallery_gen}/src/SettingsPage.tsx
            ${_gallery_gen}/src/DisplayPage.tsx ${_gallery_gen}/src/Badge.tsx ${_gallery_gen}/src/Divider.tsx ${_gallery_gen}/src/Card.tsx ${_gallery_gen}/src/Avatar.tsx ${_gallery_gen}/src/Skeleton.tsx ${_gallery_gen}/src/Spinner.tsx
    DEPENDS tessera_codegen_cli ${_gallery_settings_page} ${_gallery_controls} ${CMAKE_CURRENT_SOURCE_DIR}/ControlsPage.tsx ${CMAKE_CURRENT_SOURCE_DIR}/TagsPage.tsx ${_gallery_tags} ${_gallery_display} ${CMAKE_CURRENT_SOURCE_DIR}/DisplayPage.tsx ${_gallery_primitives} ${_gallery_button} ${_gallery_safe_area}
            ${CMAKE_CURRENT_SOURCE_DIR}/Gallery.tsx ${CMAKE_CURRENT_SOURCE_DIR}/SafeAreaPage.tsx
            ${CMAKE_CURRENT_SOURCE_DIR}/FormPage.tsx ${CMAKE_CURRENT_SOURCE_DIR}/DashPage.tsx
            ${CMAKE_CURRENT_SOURCE_DIR}/SwatchPage.tsx ${_gallery_icon} ${_gallery_icon_glyph}
            ${CMAKE_CURRENT_SOURCE_DIR}/IconsPage.tsx ${CMAKE_CURRENT_SOURCE_DIR}/ListsPage.tsx
            ${CMAKE_CURRENT_SOURCE_DIR}/TablesPage.tsx
            ${CMAKE_CURRENT_SOURCE_DIR}/form.module.css
    COMMENT "Generating the gallery page"
    VERBATIM)

# The web build (issue #1023, ADR-046): the same generated pages and sheets, driven by the web app
# driver. The generation above runs the target-built codegen CLI, which is the Node tool in an
# Emscripten build (NODERAWFS), executed through CMAKE_CROSSCOMPILING_EMULATOR. The CSS, theme and
# font are preloaded into the module's file system at /gallery; the shared resources read them there.
if (EMSCRIPTEN)
    set(_gallery_web_files
        button.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/button.css
        icon.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/icon.css
        switch.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/switch.css
        checkbox.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/checkbox.css
        radio.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/radio.css
        range.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/range.css
        progress.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/progress.css
        disclosure.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/disclosure.css
        badge.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/badge.css
        divider.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/divider.css
        card.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/card.css
        avatar.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/avatar.css
        skeleton.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/skeleton.css
        spinner.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/spinner.css
        safe-area.css=${PROJECT_SOURCE_DIR}/packages/tessera-ui/safe-area.css
        gallery.css=${CMAKE_CURRENT_SOURCE_DIR}/gallery.css
        form.module.css=${CMAKE_CURRENT_SOURCE_DIR}/form.module.css
        dash.css=${CMAKE_CURRENT_SOURCE_DIR}/dash.css
        swatch.css=${CMAKE_CURRENT_SOURCE_DIR}/swatch.css
        lists.css=${CMAKE_CURRENT_SOURCE_DIR}/lists.css
        tables.css=${CMAKE_CURRENT_SOURCE_DIR}/tables.css
        settings.css=${_gallery_settings_css}
        instrument.theme.json=${PROJECT_SOURCE_DIR}/packages/tessera-ui/themes/instrument.theme.json
        IBMPlexSans-Regular.ttf=${PROJECT_SOURCE_DIR}/packages/tessera-fonts/ibm-plex/fonts/IBMPlexSans-Regular.ttf)
    add_executable(gallery_web web_main.cpp resources.cpp form_module.cpp ${_gallery_outputs})
    target_include_directories(gallery_web PRIVATE ${_gallery_gen})
    target_link_libraries(gallery_web PRIVATE
        tessera_app_web
        tessera_scene
        tessera_text_edit
        tessera_animation
        tessera_style
        tessera_css_module)
    target_compile_definitions(gallery_web PRIVATE
        TESSERA_GALLERY_FONT="/gallery/IBMPlexSans-Regular.ttf"
        TESSERA_GALLERY_SAFE_AREA_CSS="/gallery/safe-area.css"
        TESSERA_GALLERY_BUTTON_CSS="/gallery/button.css"
        TESSERA_GALLERY_ICON_CSS="/gallery/icon.css"
        TESSERA_GALLERY_SWITCH_CSS="/gallery/switch.css"
        TESSERA_GALLERY_CHECKBOX_CSS="/gallery/checkbox.css"
        TESSERA_GALLERY_RADIO_CSS="/gallery/radio.css"
        TESSERA_GALLERY_RANGE_CSS="/gallery/range.css"
        TESSERA_GALLERY_PROGRESS_CSS="/gallery/progress.css"
        TESSERA_GALLERY_DISCLOSURE_CSS="/gallery/disclosure.css"
        TESSERA_GALLERY_DISPLAY_CSS_DIR="/gallery"
        TESSERA_GALLERY_CSS="/gallery/gallery.css"
        TESSERA_GALLERY_FORM_CSS="/gallery/form.module.css"
        TESSERA_GALLERY_DASH_CSS="/gallery/dash.css"
        TESSERA_GALLERY_SWATCH_CSS="/gallery/swatch.css"
        TESSERA_GALLERY_LISTS_CSS="/gallery/lists.css"
        TESSERA_GALLERY_TABLES_CSS="/gallery/tables.css"
        TESSERA_GALLERY_SETTINGS_CSS="/gallery/settings.css"
        TESSERA_GALLERY_THEME="/gallery/instrument.theme.json")
    target_compile_features(gallery_web PRIVATE cxx_std_23)
    tessera_web_app(gallery_web)
    # The ThorVG rasterizer, the glyph atlas and the font want more than the default stack and heap.
    target_link_options(gallery_web PRIVATE
        "SHELL:-s ENVIRONMENT=web,node"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s STACK_SIZE=1048576")
    foreach (_entry IN LISTS _gallery_web_files)
        string(REPLACE "=" ";" _pair "${_entry}")
        list(GET _pair 0 _name)
        list(GET _pair 1 _source)
        target_link_options(gallery_web PRIVATE "SHELL:--preload-file ${_source}@/gallery/${_name}")
    endforeach ()
    return()
endif ()


add_executable(gallery main.cpp golden.cpp golden.hpp form_module.cpp form_module.hpp
               resources.cpp resources.hpp ${_gallery_outputs})
target_include_directories(gallery PRIVATE ${_gallery_gen})
target_link_libraries(gallery PRIVATE
    tessera_app_window
    tessera_scene
    tessera_text_edit
    tessera_animation
    tessera_style
    tessera_css_module
    tessera_harness_capture
    tessera_stb)
target_compile_definitions(gallery PRIVATE
    TESSERA_GALLERY_FONT="${PROJECT_SOURCE_DIR}/packages/tessera-fonts/ibm-plex/fonts/IBMPlexSans-Regular.ttf"
    TESSERA_GALLERY_SAFE_AREA_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/safe-area.css"
    TESSERA_GALLERY_BUTTON_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/button.css"
    TESSERA_GALLERY_SWITCH_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/switch.css"
    TESSERA_GALLERY_CHECKBOX_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/checkbox.css"
    TESSERA_GALLERY_RADIO_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/radio.css"
    TESSERA_GALLERY_RANGE_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/range.css"
    TESSERA_GALLERY_PROGRESS_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/progress.css"
    TESSERA_GALLERY_DISCLOSURE_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/disclosure.css"
    TESSERA_GALLERY_DISPLAY_CSS_DIR="${PROJECT_SOURCE_DIR}/packages/tessera-ui"
    TESSERA_GALLERY_ICON_CSS="${PROJECT_SOURCE_DIR}/packages/tessera-ui/icon.css"
    TESSERA_GALLERY_CSS="${CMAKE_CURRENT_SOURCE_DIR}/gallery.css"
    TESSERA_GALLERY_FORM_CSS="${CMAKE_CURRENT_SOURCE_DIR}/form.module.css"
    TESSERA_GALLERY_DASH_CSS="${CMAKE_CURRENT_SOURCE_DIR}/dash.css"
    TESSERA_GALLERY_SWATCH_CSS="${CMAKE_CURRENT_SOURCE_DIR}/swatch.css"
    TESSERA_GALLERY_LISTS_CSS="${CMAKE_CURRENT_SOURCE_DIR}/lists.css"
    TESSERA_GALLERY_TABLES_CSS="${CMAKE_CURRENT_SOURCE_DIR}/tables.css"
    TESSERA_GALLERY_SETTINGS_CSS="${_gallery_settings_css}"
    TESSERA_GALLERY_THEME="${PROJECT_SOURCE_DIR}/packages/tessera-ui/themes/instrument.theme.json")
target_compile_features(gallery PRIVATE cxx_std_23)

if (TESSERA_BUILD_TESTS)
    enable_testing()
    # The page table (issue #528): every name `--page` accepts has a branch that builds it, an
    # unknown name is refused by both lists, and only the form page claims a CSS module. It needs no
    # GPU, so it runs on every lane - which matters because it is the test that catches the drift a
    # second list would introduce.
    # `page_table.hpp` includes the generated page headers, so this target must not start
    # compiling before they exist. Listing the outputs in `add_executable` is what tells CMake
    # that - the `gallery` target above does exactly that, and omitting it here left the test
    # compiling against a directory the custom command had not written yet (#900). The include
    # directory alone is not a dependency: it names where to look, not when the files appear.
    add_executable(gallery_page_table_tests page_table_tests.cpp ${_gallery_outputs})
    # The page types are generated components: a Form page pulls in the text-input runtime even
    # though the test never types, because the table instantiates every page's Scene.
    target_link_libraries(gallery_page_table_tests PRIVATE tessera_scene tessera_text_edit
        tessera_animation tessera_layout)
    target_include_directories(gallery_page_table_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${_gallery_gen})
    target_compile_features(gallery_page_table_tests PRIVATE cxx_std_23)
    add_test(NAME gallery_page_table COMMAND gallery_page_table_tests)

    # Headless interaction of Switch, Checkbox and Radio through the generated controls page (issue
    # #240): keyboard and pointer through the real InputRouter, no GPU.
    add_executable(gallery_controls_runtime_tests controls_runtime_tests.cpp ${_gallery_outputs})
    target_link_libraries(gallery_controls_runtime_tests PRIVATE tessera_scene tessera_text_edit
        tessera_animation)
    target_include_directories(gallery_controls_runtime_tests PRIVATE ${_gallery_gen})
    target_compile_features(gallery_controls_runtime_tests PRIVATE cxx_std_23)
    add_test(NAME gallery_controls_runtime COMMAND gallery_controls_runtime_tests)

    # The same through the HTML form tags (issue #1041): the generated tags page, keyboard and
    # pointer through the real InputRouter, no GPU.
    add_executable(gallery_tags_runtime_tests tags_runtime_tests.cpp ${_gallery_outputs})
    target_link_libraries(gallery_tags_runtime_tests PRIVATE tessera_scene tessera_text_edit
        tessera_animation tessera_layout)
    target_include_directories(gallery_tags_runtime_tests PRIVATE ${_gallery_gen})
    target_compile_features(gallery_tags_runtime_tests PRIVATE cxx_std_23)
    add_test(NAME gallery_tags_runtime COMMAND gallery_tags_runtime_tests)

    # The Settings starter template (issues #240, #527): the generated page, keyboard and pointer
    # through the real InputRouter, no GPU. Building it is also the codegen compile test: the
    # page and every library component it uses go through the CLI and the C++ compiler.
    add_executable(gallery_settings_runtime_tests settings_runtime_tests.cpp ${_gallery_outputs})
    target_link_libraries(gallery_settings_runtime_tests PRIVATE tessera_scene tessera_text_edit
        tessera_animation tessera_layout)
    target_include_directories(gallery_settings_runtime_tests PRIVATE ${_gallery_gen})
    target_compile_features(gallery_settings_runtime_tests PRIVATE cxx_std_23)
    add_test(NAME gallery_settings_runtime COMMAND gallery_settings_runtime_tests)
    # Headless accessibility contract of the display components (issue #240): roles, names and
    # hidden presentation boxes through the generated display page, no GPU.
    add_executable(gallery_display_runtime_tests display_runtime_tests.cpp ${_gallery_outputs})
    target_link_libraries(gallery_display_runtime_tests PRIVATE tessera_scene tessera_text_edit
        tessera_animation)
    target_include_directories(gallery_display_runtime_tests PRIVATE ${_gallery_gen})
    target_compile_features(gallery_display_runtime_tests PRIVATE cxx_std_23)
    add_test(NAME gallery_display_runtime COMMAND gallery_display_runtime_tests)

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
        "rtl|--rtl"
        # A transition caught part-way: the form, click on `Yearly`, 100 ms later. It pins the whole
        # path (click, style stage, first sample, ticks, the one AppHost paint), not just the maths.
        "form_plan_mid|--page form --click seg:1 --advance-ms 100 --width 640 --height 720")
    foreach (_theme light dark high_contrast)
        string(REPLACE "_" "-" _theme_id "${_theme}")
        foreach (_state none hover active focus)
            list(APPEND _gallery_cases "${_theme}_${_state}|--theme instrument-${_theme_id} --state ${_state}")
        endforeach ()
    endforeach ()
    # SafeArea (issue #223): one screen per scripted device, in the light and dark themes. The
    # window size is the device's: a notched phone, a gesture-navigation phone, a tablet-width
    # window with a keyboard, and a book-mode foldable.
    # The token swatch page (issue #280): every colour token of each theme. Every case paints with the
    # font pack's Plex Sans Regular, the gallery's default face (#973).
    foreach (_theme light dark high_contrast)
        string(REPLACE "_" "-" _theme_id "${_theme}")
        list(APPEND _gallery_cases
            "swatch_${_theme}|--page swatch --theme instrument-${_theme_id} --width 560 --height 360")
    endforeach ()
    # The Icon page (issue #530): every icon in both sizes, per theme, plus the right-to-left flip of
    # the directional icon on the light theme.
    foreach (_theme light dark high_contrast)
        string(REPLACE "_" "-" _theme_id "${_theme}")
        list(APPEND _gallery_cases
            "icons_${_theme}|--page icons --theme instrument-${_theme_id} --width 480 --height 280")
    endforeach ()
    list(APPEND _gallery_cases "icons_rtl|--page icons --rtl --width 480 --height 280")
    # The list look page (issue #1006): the UA markers, nesting, start/reversed/value and the
    # alphabetic and roman types, per theme.
    foreach (_theme light dark)
        list(APPEND _gallery_cases
            "lists_${_theme}|--page lists --theme instrument-${_theme} --width 760 --height 540")
    endforeach ()
    # The table look page (issue #543): a data table with a caption and collapsed borders, spans,
    # the separate model, a fixed layout and vertical alignment, per theme.
    foreach (_theme light dark)
        list(APPEND _gallery_cases
            "tables_${_theme}|--page tables --theme instrument-${_theme} --width 760 --height 540")
    endforeach ()

    # The Settings starter template (issues #240, #527): per theme, plus compact, text scale 150 and
    # the right-to-left mirror on the light theme, and the screen after a toggle and a Save click.
    foreach (_theme light dark high_contrast)
        string(REPLACE "_" "-" _theme_id "${_theme}")
        list(APPEND _gallery_cases
            "settings_${_theme}|--page settings --theme instrument-${_theme_id} --width 480 --height 640")
    endforeach ()
    list(APPEND _gallery_cases
        "settings_compact|--page settings --density compact --width 480 --height 640"
        "settings_scale_150|--page settings --text-scale 150 --width 560 --height 720"
        "settings_rtl|--page settings --rtl --width 480 --height 640")

    # The input controls page (issue #240): Switch, Checkbox and RadioGroup in every state, per theme,
    # plus the right-to-left mirror on the light theme.
    foreach (_theme light dark high_contrast)
        string(REPLACE "_" "-" _theme_id "${_theme}")
        list(APPEND _gallery_cases
            "controls_${_theme}|--page controls --theme instrument-${_theme_id} --width 640 --height 360")
    endforeach ()
    list(APPEND _gallery_cases "controls_rtl|--page controls --rtl --width 640 --height 360")
    # The form tag page (issue #1041): input checkbox/radio/range, progress, meter and details,
    # written as lowercase tags, per theme, plus the right-to-left mirror on the light theme.
    foreach (_theme light dark high_contrast)
        string(REPLACE "_" "-" _theme_id "${_theme}")
        list(APPEND _gallery_cases
            "tags_${_theme}|--page tags --theme instrument-${_theme_id} --width 640 --height 440")
    endforeach ()
    # The display page (issue #240): Badge, Divider, Card, Avatar, Skeleton and Spinner, per theme,
    # plus the right-to-left mirror on the light theme.
    foreach (_theme light dark high_contrast)
        string(REPLACE "_" "-" _theme_id "${_theme}")
        list(APPEND _gallery_cases
            "display_${_theme}|--page display --theme instrument-${_theme_id} --width 640 --height 440")
    endforeach ()
    list(APPEND _gallery_cases "display_rtl|--page display --rtl --width 640 --height 440")
    list(APPEND _gallery_cases "tags_rtl|--page tags --rtl --width 640 --height 440")
    foreach (_device notch gesture keyboard fold)
        foreach (_theme light dark)
            list(APPEND _gallery_cases
                "safe_area_${_device}_${_theme}|--page safe-area --device ${_device} --theme instrument-${_theme}")
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
