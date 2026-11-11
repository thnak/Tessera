# SPDX-License-Identifier: MIT

# Shell design capturer (docs/design/shell-design-preview.md): renders the startup P0/P2/P4 mock
# frames and the window-chrome mock frames through the real AppHost + Batched2D path. Design mock,
# not the phase scheduler or the Windows custom title bar.
add_executable(design_mock
    main.cpp)

target_link_libraries(design_mock PRIVATE
    tessera_app_window
    tessera_stb)

target_compile_definitions(design_mock PRIVATE
    TESSERA_DESIGN_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")

target_compile_features(design_mock PRIVATE cxx_std_23)

if (TESSERA_BUILD_TESTS)
    enable_testing()
    add_test(NAME design_mock
        COMMAND design_mock ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(design_mock PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")
endif()
