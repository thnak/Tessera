# SPDX-License-Identifier: MIT

add_library(tessera_scene STATIC
    node.cpp
    node_metadata.cpp
    overlay_dismiss.cpp
    overlay_host.cpp
    overlay_place.cpp
    overlay_placement.cpp
    event.cpp
    click_synthesis.cpp
    dirty_pass.cpp
    focus.cpp
    focus_scope.cpp
    hover_dispatch.cpp
    input_dispatch.cpp
    input_dispatch_click.cpp
    scroll.cpp
    scroll_animation.cpp
    text_input_dispatch.cpp
    update_scheduler.cpp
    wheel_dispatch.cpp)

target_include_directories(tessera_scene PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_scene PUBLIC tessera_rhi tessera_input)

target_compile_features(tessera_scene PUBLIC cxx_std_23)

# Scroll style properties wired to the scroll controller (issue #248). Registered here, not in the
# shared tests/unit registry, so a lane adding tests never edits a file another lane is working in.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(scroll_properties_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/scroll_properties_tests.cpp)
    target_link_libraries(scroll_properties_tests PRIVATE tessera_layout tessera_style)
    add_test(NAME scroll_properties COMMAND scroll_properties_tests)
endif()
