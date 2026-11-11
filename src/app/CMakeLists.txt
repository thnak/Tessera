# SPDX-License-Identifier: MIT

# The app host (docs/design/app-host.md): the production frame loop that composes style, layout,
# scene sync, damage and paint for one scene root. It never touches a window, swapchain or GPU
# device; window runners and headless tests drive it.
add_library(tessera_app STATIC
    host.cpp)

target_include_directories(tessera_app PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_app PUBLIC
    tessera_frame
    tessera_layout
    tessera_animation
    tessera_paint
    tessera_style
    tessera_scene
    tessera_runtime)

target_compile_features(tessera_app PUBLIC cxx_std_23)
