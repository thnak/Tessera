# SPDX-License-Identifier: MIT

# The app host (docs/design/app-host.md): the production frame loop that composes style, layout,
# scene sync, damage and paint for one scene root. It never touches a window, swapchain or GPU
# device; window runners and headless tests drive it.
add_library(tessera_app STATIC
    host.cpp
    host_impl.hpp
    host_theme.cpp
    safe_area_state.cpp
    safe_area_state.hpp
    system_motion.cpp
    window_config.cpp
    window_config.hpp)

target_include_directories(tessera_app PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# The adaptive-quality controller is private runtime state (src/runtime/adaptive_quality.hpp, like
# this module's own window_config.hpp); the host owns it but app code never sees the type.
target_include_directories(tessera_app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/runtime)

target_link_libraries(tessera_app PUBLIC
    tessera_frame
    tessera_layout
    tessera_animation
    tessera_paint
    tessera_style
    tessera_scene
    tessera_runtime)

target_compile_features(tessera_app PUBLIC cxx_std_23)

if (WIN32)
    target_link_libraries(tessera_app PRIVATE user32)
endif ()

# The window driver: owns the platform window, Vulkan device/swapchain, Batched2D renderer and the
# font/text stack, and runs the AppHost loop against real input and presentation. Vulkan is the
# native backend (WebGPU is the Wasm/design-time one), and window presentation is host-only, so the
# target exists only for host builds with the Vulkan backend.
if (TESSERA_BUILD_VULKAN AND NOT CMAKE_CROSSCOMPILING)
    add_library(tessera_app_window STATIC
        frame_capture.cpp
        window.cpp
        window_backdrop_request.cpp
        window_backdrop_request.hpp
        window_impl.hpp
        window_internal.hpp
        window_lifecycle.cpp
        window_recovery.cpp)

    target_include_directories(tessera_app_window PUBLIC
        ${PROJECT_SOURCE_DIR}/include)
    target_include_directories(tessera_app_window PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(tessera_app_window PUBLIC
        tessera_app
        tessera_gpu
        tessera_hal_window
        tessera_input_pump
        tessera_paint
        tessera_rhi_vulkan
        tessera_scale
        tessera_text
        tessera_text_edit
        tessera_text_render)

    target_compile_features(tessera_app_window PUBLIC cxx_std_23)
endif()
