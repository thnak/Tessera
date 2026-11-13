# SPDX-License-Identifier: MIT

# Interactive UI acceptance for the app-host lane: the generated Demo component + a compiled
# stylesheet + AppHost + the Batched2D backend. The capture mode is windowless (offscreen target +
# readback) so CI can run it on any Vulkan device; the windowed mode is a manual visual check.
add_executable(click_demo
    main.cpp
    scene.hpp)

target_link_libraries(click_demo PRIVATE
    tessera_app_window
    tessera_compiler_demo
    tessera_stb)

target_compile_definitions(click_demo PRIVATE
    TESSERA_SAMPLE_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")

target_compile_features(click_demo PRIVATE cxx_std_23)

if (TESSERA_BUILD_TESTS)
    # This directory is added before the root registers ctest, so register the test here.
    enable_testing()
    add_test(NAME click_demo_capture
        COMMAND click_demo --capture ${CMAKE_CURRENT_BINARY_DIR}/click_demo.png)
    # No Vulkan device: the tool prints its documented skip line and exits 0.
    set_tests_properties(click_demo_capture PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")
endif()
