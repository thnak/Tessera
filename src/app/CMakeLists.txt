# SPDX-License-Identifier: MIT

# The app host (docs/design/app-host.md): the production frame loop that composes style, layout,
# scene sync, damage and paint for one scene root. It never touches a window, swapchain or GPU
# device; window runners and headless tests drive it.
add_library(tessera_app STATIC
    host.cpp
    # BackRequested dispatch (issue #445): overlay, then route handler, then unhandled.
    host_back.cpp
    host_impl.hpp
    # The adaptive-quality ladder of AppHost::Impl (god-type audit slice 10): the controller, its
    # diagnostics recording, and the two application rules a decision implies.
    quality_stage.cpp
    quality_stage.hpp
    # The adapters binding that stage to the real host state. Kept out of host.cpp, which is already
    # at its size budget.
    host_quality.cpp
    host_theme.cpp
    # The OS appearance rule of issue #187 (`AppHost::set_appearance`): a media-affecting change
    # re-resolves the cascade. Split out of host.cpp, which is at its size budget.
    host_appearance.cpp
    # The viewport rule of issue #436 (`resize` / `set_window_metrics`): the window IS the media
    # context, and the cascade is re-resolved only when a `@media` answer can actually change.
    host_viewport.cpp
    # Whether the selector tree and its memo survive a restyle, and which subtrees recompute.
    host_style_tree.cpp
    # The layout-unchanged gate (issue #735, slice A): when a restyle provably kept every layout
    # input, reuse the previous result instead of rebuilding the layout input and hashing it.
    # Split out of host.cpp, which is at its size budget.
    host_layout_gate.cpp
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
    accessibility_host_impl.hpp
    # The two WindowHost::Impl state clusters that carry policy but no lifecycle coupling
    # (god-type audit slice 9): the GPU frame-time signal and the adaptive render-scale targets.
    # Both need only the RHI seam, so they live in this always-built library (not the Vulkan-gated
    # window driver) and a null-device test drives them.
    frame_time_probe.cpp
    frame_time_probe.hpp
    scaled_targets.cpp
    scaled_targets.hpp)

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
    # The RHI seam only: ScaledTargets creates offscreen render targets through it, and stays
    # backend-agnostic (the null backend in tests, Vulkan in the window driver).
    tessera_rhi
    # The display-aware AA resolver (`window_config.cpp`, docs/design/display-aware-rendering.md).
    tessera_scale
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
        # The window's display-aware AA policy resolution and application (display-aware-rendering
        # section 6), shared by creation and the metrics-change path.
        window_aa.cpp
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
        window_lifecycle_seam.cpp
        # The frame's paint-target decision (adaptive render scale, glass, 4x MSAA presentation),
        # split out of window.cpp for its size budget.
        window_present_targets.cpp
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
