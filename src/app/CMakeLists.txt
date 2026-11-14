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
    window_config.hpp
    # The one accessibility controller WindowHost owns (issues #185/#186). It is platform-neutral
    # C++ over the HAL window seam and the accessibility bridges, so it lives in this always-built
    # library (not the Vulkan-gated window driver) and the app-level tests drive it with fakes.
    accessibility_host.cpp
    accessibility_host_atspi.cpp
    accessibility_host_facade.cpp
    accessibility_host.hpp
    accessibility_host_impl.hpp)

target_include_directories(tessera_app PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# The adaptive-quality controller is private runtime state (src/runtime/adaptive_quality.hpp, like
# this module's own window_config.hpp); the host owns it but app code never sees the type.
target_include_directories(tessera_app PRIVATE
    ${PROJECT_SOURCE_DIR}/src/runtime)

target_link_libraries(tessera_app PUBLIC
    tessera_accessibility
    tessera_frame
    tessera_hal_window
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
        # WindowHost's accessibility adapters (issues #185/#186): the paint->a11y style provider and
        # the action execution on the scene, outside window.cpp's size budget.
        window_accessibility.cpp
        window_backdrop_request.cpp
        window_backdrop_request.hpp
        window_cursor.cpp
        window_cursor.hpp
        window_impl.hpp
        window_internal.hpp
        window_lifecycle.cpp
        window_recovery.cpp)

    target_include_directories(tessera_app_window PUBLIC
        ${PROJECT_SOURCE_DIR}/include)
    target_include_directories(tessera_app_window PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(tessera_app_window PUBLIC
        tessera_accessibility
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
